#pragma once

#include "constants.hpp"
#include "contiguousMemory.hpp"
#include "structs.hpp"
#include "utils.hpp"
#include "vmexitHandler.hpp"
#include <intrin.h>
#include <ntddk.h>

// preventing an else statement from being attached to the if statment in this macro with a do while "loop"
#define VMCS_WRITE_SAFE(Field, Value)                                                                  \
    do                                                                                                 \
    {                                                                                                  \
        if (__vmx_vmwrite(static_cast<UINT64>(Field), static_cast<UINT64>(Value)) != 0)                \
        {                                                                                              \
            LOG_ERROR("Failed to write %s to the VMCS region on core %lu.", #Field, m_processorIndex); \
            return false;                                                                              \
        }                                                                                              \
    } while (false)

class Vcpu;

extern "C" bool AsmVirtualize(Vcpu* Context);
extern "C" bool AsmVmExitHandler(Vcpu* Context);

class Vcpu
{
private:
    ULONG m_processorIndex;
    EPT_POINTER m_eptPointer;
    Optional<ContiguousMemory> m_vmxon;
    Optional<ContiguousMemory> m_vmcs;
    Optional<ContiguousMemory> m_msrBitmap;
    PVOID m_hypervisorStack = nullptr;

    void EnableVmx()
    {
        const unsigned long long oldCr4 = __readcr4();

        __writecr4(oldCr4 | CR4_FLAGS::VMXE);
    }

    void DisableVmx()
    {
        const unsigned long long oldCr4 = __readcr4();

        __writecr4(oldCr4 & (~CR4_FLAGS::VMXE));
    }

public:
    Vcpu() = default;
    ~Vcpu() = default;

    // needs to run on the specific core this VCPU object is assigned to
    bool Initialize(const ULONG processorIndex, const EPT_POINTER eptPointer)
    {
        m_processorIndex = processorIndex;
        m_eptPointer = eptPointer;

        IA32_FEATURE_CONTROL_MSR featureControl = { 0 };
        featureControl.All = __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_FEATURE_CONTROL));

        if (featureControl.Fields.Lock == FALSE)
        {
            featureControl.Fields.Lock = TRUE;
            featureControl.Fields.EnableVMXON = TRUE;

            __writemsr(static_cast<ULONG>(SYSTEM_MSR::IA32_FEATURE_CONTROL), featureControl.All);
        }
        else if (featureControl.Fields.EnableVMXON == FALSE)
        {
            LOG_ERROR("VMX locked off by BIOS on core %lu.", m_processorIndex);

            return false;
        }

        EnableVmx();

        m_vmxon = ContiguousMemory::allocate(PAGE_SIZE);
        if (!m_vmxon.has())
        {
            LOG_ERROR("Failed to allocate contiguous memory for the VMXON region.");

            return false;
        }

        IA32_VMX_BASIC_MSR vmxBasic = { 0 };

        vmxBasic.All = __readmsr(static_cast<ULONG>(VMX_MSR::IA32_BASIC));
        ULONG revisionId = static_cast<ULONG>(vmxBasic.All);

        // writing the revision ID into the vmxon memory, this is necessary to ensure everything is compatible
        *(reinterpret_cast<ULONG*>(m_vmxon.value().VirtualAddress())) = revisionId;

        if (__vmx_on(&m_vmxon.value().PhysicalAddress()) != VMX_RESULT::SUCCESS)
        {
            LOG_ERROR("Failed to execute the __vmx_on() intrinsic.");

            return false;
        }

        m_vmcs = ContiguousMemory::allocate(PAGE_SIZE);
        if (!m_vmcs.has())
        {
            LOG_ERROR("Failed to allocate contiguous memory for the VMCS region.");

            return false;
        }

        *(reinterpret_cast<ULONG*>(m_vmcs.value().VirtualAddress())) = revisionId;

        if (__vmx_vmclear(&m_vmcs.value().PhysicalAddress()) != VMX_RESULT::SUCCESS)
        {
            LOG_ERROR("Failed to execute the __vmx_vmclear() intrinsic.");

            return false;
        }

        if (__vmx_vmptrld(&m_vmcs.value().PhysicalAddress()) != VMX_RESULT::SUCCESS)
        {
            LOG_ERROR("Failed to execute the __vmx_vmptrld() intrinsic.");

            return false;
        }

        m_msrBitmap = ContiguousMemory::allocate(PAGE_SIZE);
        if (!m_msrBitmap.has())
        {
            LOG_ERROR("Failed to allocate contiguous memory for the MSR Bitmap.");

            return false;
        }

        m_hypervisorStack = ExAllocatePool2(
            POOL_FLAG_NON_PAGED, HYPERVISOR_CONFIG::STACK_SIZE, HYPERVISOR_CONFIG::STACK_TAG);
        if (!m_hypervisorStack)
        {
            LOG_ERROR("Failed to allocate host stack.");

            return false;
        }

        LOG_INFO("VCPU %lu successfully initialized.", m_processorIndex);

        // setting up the VMCS and calling vmlaunch
        if (!AsmVirtualize(this))
        {
            UINT64 vmInstructionError = VmcsRead(VMCS_FIELDS::VM_INSTRUCTION_ERROR);

            LOG_ERROR("Failed to virtualize on core %lu. VM_INSTRUCTION_ERROR=%llu",
                      m_processorIndex, vmInstructionError);

            return false;
        }

        return true;
    }

    void Teardown()
    {
        AsmVmcall(HYPERVISOR_CONFIG::SHUTDOWN_HYPERCALL, 0, 0, 0);

        m_vmxon.clear();
        m_vmcs.clear();
        m_msrBitmap.clear();

        if (m_hypervisorStack != nullptr)
        {
            ExFreePoolWithTag(m_hypervisorStack, HYPERVISOR_CONFIG::STACK_TAG);
            m_hypervisorStack = nullptr;
        }

        // we already did this in the assembly code but i left it here just for safety
        DisableVmx();

        LOG_INFO("VCPU %lu successfully powered down and memory freed.", m_processorIndex);
    }

    static bool AdjustControlValue(ULONG requestedValue, ULONG64 msrValue, ULONG* outAdjustedValue)
    {
        LARGE_INTEGER msr;
        msr.QuadPart = msrValue;

        // bits the caller requested that the hardware doesn't allow
        ULONG disallowedRequested = requestedValue & ~msr.HighPart;
        if (disallowedRequested != 0)
        {
            LOG_ERROR("Requested control bits 0x%lX not supported by hardware (allowed mask = 0x%lX).",
                      disallowedRequested, msr.HighPart);
            return false;
        }

        ULONG finalValue = requestedValue & msr.HighPart;
        finalValue |= msr.LowPart; // forcing required 1 bits
        *outAdjustedValue = finalValue;

        return true;
    }

    static SEGMENT_INFO GetSegmentInfo(SEGMENT_SELECTOR selector, ULONG64 gdtBase)
    {
        SEGMENT_INFO segmentInfo = { 0 };

        if (selector.Fields.Index == GDT_CONSTANTS::NULL_SELECTOR_INDEX)
        {
            segmentInfo.AccessRights = SEGMENT_ACCESS_RIGHTS::UNUSABLE;
            return segmentInfo;
        }

        SEGMENT_DESCRIPTOR* segmentDescriptor = reinterpret_cast<SEGMENT_DESCRIPTOR*>(
            gdtBase + selector.Fields.Index * GDT_CONSTANTS::GDT_ENTRY_SIZE);

        segmentInfo.Base = segmentDescriptor->Fields.BaseLow |
                           (segmentDescriptor->Fields.BaseMiddle << SEGMENT_SHIFTS::BASE_MIDDLE) |
                           (segmentDescriptor->Fields.BaseHigh << SEGMENT_SHIFTS::BASE_HIGH);

        // system segments in 64 bit are 16 bytes long
        if (segmentDescriptor->Fields.System == GDT_CONSTANTS::SYSTEM_SEGMENT_FLAG)
        {
            SYSTEM_SEGMENT_DESCRIPTOR_64* sysDescriptor = reinterpret_cast<SYSTEM_SEGMENT_DESCRIPTOR_64*>(
                segmentDescriptor);

            segmentInfo.Base |= (static_cast<ULONG64>(sysDescriptor->BaseUpper32) << BITS_32::HIGH_SHIFT);
        }

        segmentInfo.Limit = static_cast<ULONG32>(
            segmentDescriptor->Fields.LimitLow |
            (segmentDescriptor->Fields.LimitHigh << SEGMENT_SHIFTS::LIMIT_HIGH));

        if (segmentDescriptor->Fields.Granularity)
        {
            segmentInfo.Limit = segmentInfo.Limit * PAGE_SIZE + (PAGE_SIZE - 1);
        }

        segmentInfo.AccessRights |= (segmentDescriptor->Fields.Type << SEGMENT_SHIFTS::AR_TYPE);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.System << SEGMENT_SHIFTS::AR_SYSTEM);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.DPL << SEGMENT_SHIFTS::AR_DPL);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.Present << SEGMENT_SHIFTS::AR_PRESENT);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.AVL << SEGMENT_SHIFTS::AR_AVL);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.LongMode << SEGMENT_SHIFTS::AR_LONG_MODE);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.DefaultBig << SEGMENT_SHIFTS::AR_DEFAULT_BIG);
        segmentInfo.AccessRights |= (segmentDescriptor->Fields.Granularity << SEGMENT_SHIFTS::AR_GRANULARITY);
        segmentInfo.AccessRights &= ~(1ul << SEGMENT_SHIFTS::AR_UNUSABLE);

        return segmentInfo;
    }

    // the guest registers given as parameters will be obtained via the assembly code
    bool SetupVmcs(const UINT64 GuestRsp, const UINT64 GuestRip)
    {
        // turning on selected features for the hypervisor

        ULONG pinBasedRequest = 0; // we don't need any special pin features

        ULONG pinBasedControls = 0;
        if (!AdjustControlValue(
                pinBasedRequest,
                __readmsr(static_cast<ULONG>(VMX_MSR::IA32_TRUE_PINBASED_CTLS)),
                &pinBasedControls))
        {
            LOG_ERROR("CPU does not support the requested Pin-Based Controls on core %lu.", m_processorIndex);
            return false;
        }
        VMCS_WRITE_SAFE(VMCS_FIELDS::PIN_BASED_VM_EXEC_CONTROL, pinBasedControls);

        ULONG primaryControlsRequest = 0;

        primaryControlsRequest |= PRIMARY_CONTROLS::USE_MSR_BITMAPS; // we want to intercept LSTAR/syscall checks
        primaryControlsRequest |= PRIMARY_CONTROLS::ACTIVATE_SECONDARY_CONTROLS;

        ULONG primaryControls = 0;
        if (!AdjustControlValue(
                primaryControlsRequest,
                __readmsr(static_cast<ULONG>(VMX_MSR::IA32_TRUE_PROCBASED_CTLS)), &primaryControls))
        {
            LOG_ERROR("CPU does not support the requested Primary Controls on core %lu.", m_processorIndex);
            return false;
        }
        VMCS_WRITE_SAFE(VMCS_FIELDS::PRIMARY_CPU_BASED_VM_EXEC_CONTROL, primaryControls);

        ULONG secondaryControlsRequest = 0;

        secondaryControlsRequest |= SECONDARY_CONTROLS::ENABLE_EPT;
        secondaryControlsRequest |= SECONDARY_CONTROLS::ENABLE_VPID;
        secondaryControlsRequest |= SECONDARY_CONTROLS::ENABLE_RDTSCP;
        secondaryControlsRequest |= SECONDARY_CONTROLS::ENABLE_INVPCID;
        secondaryControlsRequest |= SECONDARY_CONTROLS::ENABLE_XSAVES;

        ULONG secondaryControls = 0;
        if (!AdjustControlValue(
                secondaryControlsRequest,
                __readmsr(static_cast<ULONG>(VMX_MSR::IA32_PROCBASED_CTLS2)), &secondaryControls))
        {
            LOG_ERROR("CPU does not support the requested Secondary Controls on core %lu.", m_processorIndex);
            return false;
        }
        VMCS_WRITE_SAFE(VMCS_FIELDS::SECONDARY_CPU_BASED_VM_EXEC_CONTROL, secondaryControls);

        VMCS_WRITE_SAFE(VMCS_FIELDS::EPT_POINTER, m_eptPointer.All);

        VMCS_WRITE_SAFE(VMCS_FIELDS::VIRTUAL_PROCESSOR_ID, m_processorIndex + 1); // VPID 0 is reserved

        // if not using VMCS shadowing, intel requires this to equal INVALID_POINTER
        VMCS_WRITE_SAFE(VMCS_FIELDS::VMCS_LINK_POINTER, PHYSICAL_MEMORY::INVALID_POINTER);

        VMCS_WRITE_SAFE(VMCS_FIELDS::MSR_BITMAP, m_msrBitmap.value().PhysicalAddress());

        // ensuring everything stays 64 bit

        ULONG exitRequest = 0;
        exitRequest |= EXIT_CONTROLS::HOST_ADDRESS_SPACE_SIZE;

        ULONG exitControls = 0;
        if (!AdjustControlValue(
                exitRequest,
                __readmsr(static_cast<ULONG>(VMX_MSR::IA32_TRUE_EXIT_CTLS)),
                &exitControls))
        {
            LOG_ERROR("CPU does not support the requested VM-Exit Controls on core %lu.", m_processorIndex);
            return false;
        }
        VMCS_WRITE_SAFE(VMCS_FIELDS::VM_EXIT_CONTROLS, exitControls);

        ULONG entryRequest = 0;
        entryRequest |= ENTRY_CONTROLS::IA32E_MODE_GUEST;

        ULONG entryControls = 0;
        if (!AdjustControlValue(
                entryRequest,
                __readmsr(static_cast<ULONG>(VMX_MSR::IA32_TRUE_ENTRY_CTLS)),
                &entryControls))
        {
            LOG_ERROR("CPU does not support the requested VM-Entry Controls on core %lu.", m_processorIndex);
            return false;
        }
        VMCS_WRITE_SAFE(VMCS_FIELDS::VM_ENTRY_CONTROLS, entryControls);

        // setting up guest & host registers

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CR0, __readcr0());
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CR3, __readcr3());
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CR4, __readcr4());

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_RSP, GuestRsp);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_RIP, GuestRip);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_RFLAGS, __readeflags());

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_DR7, __readdr(7));

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_CR0, __readcr0());
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_CR3, __readcr3());
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_CR4, __readcr4());

        static constexpr UINT64 NO_BITS = 0;
        VMCS_WRITE_SAFE(VMCS_FIELDS::CR0_GUEST_HOST_MASK, NO_BITS); // let the guest own all CR0 bits for now
        VMCS_WRITE_SAFE(VMCS_FIELDS::CR0_READ_SHADOW, __readcr0());
        VMCS_WRITE_SAFE(VMCS_FIELDS::CR4_GUEST_HOST_MASK, CR4_FLAGS::VMXE);            // we own VMXE
        VMCS_WRITE_SAFE(VMCS_FIELDS::CR4_READ_SHADOW, __readcr4() & ~CR4_FLAGS::VMXE); // lying about VMXE

        // masking out RPL and TI
        static constexpr USHORT HOST_SEGMENT_SELECTOR_MASK = 0xFFF8;

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_CS_SELECTOR, AsmGetCs() & HOST_SEGMENT_SELECTOR_MASK);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_DS_SELECTOR, AsmGetDs() & HOST_SEGMENT_SELECTOR_MASK);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_ES_SELECTOR, AsmGetEs() & HOST_SEGMENT_SELECTOR_MASK);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_FS_SELECTOR, AsmGetFs() & HOST_SEGMENT_SELECTOR_MASK);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_GS_SELECTOR, AsmGetGs() & HOST_SEGMENT_SELECTOR_MASK);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_SS_SELECTOR, AsmGetSs() & HOST_SEGMENT_SELECTOR_MASK);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_TR_SELECTOR, AsmGetTr() & HOST_SEGMENT_SELECTOR_MASK);

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_FS_BASE, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_FS_BASE)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_GS_BASE, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_GS_BASE)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IA32_SYSENTER_CS, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_SYSENTER_CS)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IA32_SYSENTER_ESP, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_SYSENTER_ESP)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IA32_SYSENTER_EIP, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_SYSENTER_EIP)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SYSENTER_CS, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_SYSENTER_CS)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SYSENTER_ESP, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_SYSENTER_ESP)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SYSENTER_EIP, __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_SYSENTER_EIP)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_RSP, reinterpret_cast<UINT64>(m_hypervisorStack) + HYPERVISOR_CONFIG::STACK_SIZE);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_RIP, reinterpret_cast<UINT64>(AsmVmExitHandler));

        // setting up guest segment registers

        SYSTEM_DESCRIPTOR_TABLE_REGISTER gdtr = { 0 };
        AsmGetGdtr(&gdtr);
        ULONG64 gdtBase = gdtr.Base;

        SEGMENT_SELECTOR cs = { AsmGetCs() };
        SEGMENT_SELECTOR ds = { AsmGetDs() };
        SEGMENT_SELECTOR es = { AsmGetEs() };
        SEGMENT_SELECTOR fs = { AsmGetFs() };
        SEGMENT_SELECTOR gs = { AsmGetGs() };
        SEGMENT_SELECTOR ss = { AsmGetSs() };
        SEGMENT_SELECTOR tr = { AsmGetTr() };
        SEGMENT_SELECTOR ldtr = { AsmGetLdtr() };

        SEGMENT_INFO csInfo = GetSegmentInfo(cs, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CS_SELECTOR, cs.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CS_LIMIT, csInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CS_ACCESS_RIGHTS, csInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_CS_BASE, csInfo.Base);

        SEGMENT_INFO dsInfo = GetSegmentInfo(ds, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_DS_SELECTOR, ds.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_DS_LIMIT, dsInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_DS_ACCESS_RIGHTS, dsInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_DS_BASE, dsInfo.Base);

        SEGMENT_INFO esInfo = GetSegmentInfo(es, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_ES_SELECTOR, es.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_ES_LIMIT, esInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_ES_ACCESS_RIGHTS, esInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_ES_BASE, esInfo.Base);

        SEGMENT_INFO ssInfo = GetSegmentInfo(ss, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SS_SELECTOR, ss.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SS_LIMIT, ssInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SS_ACCESS_RIGHTS, ssInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SS_BASE, ssInfo.Base);

        SEGMENT_INFO trInfo = GetSegmentInfo(tr, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_TR_SELECTOR, tr.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_TR_LIMIT, trInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_TR_ACCESS_RIGHTS, trInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_TR_BASE, trInfo.Base);

        SEGMENT_INFO ldtrInfo = GetSegmentInfo(ldtr, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_LDTR_SELECTOR, ldtr.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_LDTR_LIMIT, ldtrInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_LDTR_ACCESS_RIGHTS, ldtrInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_LDTR_BASE, ldtrInfo.Base);

        SEGMENT_INFO fsInfo = GetSegmentInfo(fs, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_FS_SELECTOR, fs.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_FS_LIMIT, fsInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_FS_ACCESS_RIGHTS, fsInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_FS_BASE,
                        __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_FS_BASE)));

        SEGMENT_INFO gsInfo = GetSegmentInfo(gs, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_SELECTOR, gs.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_LIMIT, gsInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_ACCESS_RIGHTS, gsInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_BASE,
                        __readmsr(static_cast<ULONG>(SYSTEM_MSR::IA32_GS_BASE)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GDTR_BASE, gdtr.Base);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GDTR_LIMIT, gdtr.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_GDTR_BASE, gdtr.Base);

        // setting up the IDT

        SYSTEM_DESCRIPTOR_TABLE_REGISTER idtr = { 0 };
        AsmGetIdtr(&idtr);

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_IDTR_BASE, idtr.Base);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_IDTR_LIMIT, idtr.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IDTR_BASE, idtr.Base);

        // setting up the TR

        SEGMENT_INFO hostTrInfo = GetSegmentInfo({ AsmGetTr() }, gdtr.Base);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_TR_BASE, hostTrInfo.Base);

        return true;
    }
};

extern "C" bool SetupVmcsThunk(Vcpu* Context, UINT64 GuestRsp, UINT64 GuestRip)
{
    if (Context == nullptr)
    {
        return false;
    }

    return Context->SetupVmcs(GuestRsp, GuestRip);
}
