#pragma once

#include "constants.hpp"
#include "raii.hpp"
#include "structs.hpp"
#include "utils.hpp"
#include "vmexitHandler.hpp"
#include "vmxCapabilities.hpp"
#include <intrin.h>
#include <ntifs.h>

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
extern "C" bool AsmVmExitHandler();

class Vcpu
{
private:
    ULONG m_processorIndex = 0;
    EPT_POINTER m_eptPointer = {};
    UINT64 m_hostCr3 = 0;

    Optional<ContiguousMemory> m_vmxon;
    Optional<ContiguousMemory> m_vmcs;
    Optional<ContiguousMemory> m_msrBitmap;
    Optional<PoolBuffer> m_hypervisorStack;
    Optional<HostGdt> m_hostGdt;

    bool m_vmxeEnabled = false;
    bool m_vmxonExecuted = false;
    bool m_isLaunched = false;
    bool m_useTrueControlMsrs = false;

    Vcpu() = default;

    void EnableVmx() noexcept
    {
        __writecr4(__readcr4() | CR4_FLAGS::VMXE);
        m_vmxeEnabled = true;
    }

    void DisableVmx() noexcept
    {
        __writecr4(__readcr4() & ~CR4_FLAGS::VMXE);
        m_vmxeEnabled = false;
    }

    UINT64 ReadControlCapabilityMsr(IA32_VMX_MSR trueMsr, IA32_VMX_MSR legacyMsr) const noexcept
    {
        return VmxCapabilities::ReadControlMsr(m_useTrueControlMsrs, trueMsr, legacyMsr);
    }

    static bool IsYmmStateUsable() noexcept
    {
        int cpuInfo[CPUID_REGISTER::COUNT] = { 0 };
        __cpuid(cpuInfo, CPUID_LEAF::VERSION_AND_FEATURES);

        // OSXSAVE must be tested first as _xgetbv is itself illegal without it
        if ((cpuInfo[CPUID_REGISTER::ECX] & CPUID_FEATURES::OSXSAVE) == 0)
        {
            return false;
        }

        if ((cpuInfo[CPUID_REGISTER::ECX] & CPUID_FEATURES::AVX) == 0)
        {
            return false;
        }

        const UINT64 xcr0 = _xgetbv(XCR0::INDEX);
        return (xcr0 & XCR0::AVX_STATE) == XCR0::AVX_STATE;
    }

public:
    ~Vcpu() noexcept
    {
        const bool didTeardown = m_isLaunched || m_vmxonExecuted || m_vmxeEnabled;

        if (m_isLaunched)
        {
            const UINT64 vmcsPhysicalAddress =
                m_vmcs.has() ? m_vmcs.value().PhysicalAddress() : 0;

            // the hypercall handles invalid addresses (0) accordingly
            AsmVmcall(HYPERVISOR_CONFIG::SHUTDOWN_HYPERCALL, vmcsPhysicalAddress, 0, 0);
        }
        else if (m_vmxonExecuted)
        {
            if (m_vmcs.has())
            {
                __vmx_vmclear(&m_vmcs.value().PhysicalAddress());
            }
            __vmx_off();
        }

        if (m_vmxeEnabled)
        {
            DisableVmx();
        }

        if (didTeardown)
        {
            LOG_INFO("VCPU %lu successfully powered down and memory freed.", m_processorIndex);
        }
    }

    Vcpu(const Vcpu&) = delete;
    Vcpu& operator=(const Vcpu&) = delete;

    Vcpu(Vcpu&& other) noexcept
        : m_processorIndex(other.m_processorIndex),
          m_eptPointer(other.m_eptPointer),
          m_vmxon(static_cast<Optional<ContiguousMemory>&&>(other.m_vmxon)),
          m_vmcs(static_cast<Optional<ContiguousMemory>&&>(other.m_vmcs)),
          m_msrBitmap(static_cast<Optional<ContiguousMemory>&&>(other.m_msrBitmap)),
          m_hypervisorStack(static_cast<Optional<PoolBuffer>&&>(other.m_hypervisorStack)),
          m_hostGdt(static_cast<Optional<HostGdt>&&>(other.m_hostGdt)),
          m_vmxeEnabled(other.m_vmxeEnabled), m_vmxonExecuted(other.m_vmxonExecuted),
          m_isLaunched(other.m_isLaunched), m_useTrueControlMsrs(other.m_useTrueControlMsrs),
          m_hostCr3(other.m_hostCr3)
    {
        other.m_vmxeEnabled = false;
        other.m_vmxonExecuted = false;
        other.m_isLaunched = false;
        other.m_useTrueControlMsrs = false;
        other.m_hostCr3 = 0;
    }

    Vcpu& operator=(Vcpu&&) = delete;

    // needs to run while pinned to the specific core this VCPU object will be assigned to
    static Optional<Vcpu> Create(const ULONG processorIndex, const EPT_POINTER eptPointer, const UINT64 hostCr3)
    {
        if (!IsYmmStateUsable())
        {
            LOG_ERROR("Core %lu cannot use YMM state (AVX / OSXSAVE / XCR0); "
                      "the VM-exit handler's save-restore block requires it.",
                      processorIndex);

            return Optional<Vcpu>();
        }

        Vcpu vcpu;
        vcpu.m_processorIndex = processorIndex;
        vcpu.m_eptPointer = eptPointer;
        vcpu.m_hostCr3 = hostCr3;

        IA32_FEATURE_CONTROL_MSR featureControl = { 0 };
        featureControl.All = __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::FEATURE_CONTROL));

        if (featureControl.Fields.Lock == FALSE)
        {
            featureControl.Fields.Lock = TRUE;
            featureControl.Fields.EnableVMXON = TRUE;
            __writemsr(static_cast<ULONG>(IA32_SYSTEM_MSR::FEATURE_CONTROL), featureControl.All);
        }
        else if (featureControl.Fields.EnableVMXON == FALSE)
        {
            LOG_ERROR("VMX locked off by BIOS on core %lu.", processorIndex);
            return Optional<Vcpu>();
        }

        vcpu.EnableVmx();

        vcpu.m_vmxon = ContiguousMemory::allocate(PAGE_SIZE);
        if (!vcpu.m_vmxon.has())
        {
            LOG_ERROR("Failed to allocate contiguous memory for the VMXON region.");
            return Optional<Vcpu>();
        }

        IA32_VMX_BASIC_MSR vmxBasic = { 0 };
        vmxBasic.All = __readmsr(static_cast<ULONG>(IA32_VMX_MSR::BASIC));
        ULONG revisionId = static_cast<ULONG>(vmxBasic.All);

        // writing the revision ID into the vmxon memory, this is necessary to ensure everything is compatible
        *(reinterpret_cast<ULONG*>(vcpu.m_vmxon.value().VirtualAddress())) = revisionId;

        vcpu.m_useTrueControlMsrs = vmxBasic.Fields.SupportsTrueControlMsrs != 0;

        if (__vmx_on(&vcpu.m_vmxon.value().PhysicalAddress()) != VMX_RESULT::SUCCESS)
        {
            LOG_ERROR("Failed to execute the __vmx_on() intrinsic.");
            return Optional<Vcpu>();
        }
        vcpu.m_vmxonExecuted = true;

        vcpu.m_vmcs = ContiguousMemory::allocate(PAGE_SIZE);
        if (!vcpu.m_vmcs.has())
        {
            LOG_ERROR("Failed to allocate contiguous memory for the VMCS region.");
            return Optional<Vcpu>();
        }

        *(reinterpret_cast<ULONG*>(vcpu.m_vmcs.value().VirtualAddress())) = revisionId;

        if (__vmx_vmclear(&vcpu.m_vmcs.value().PhysicalAddress()) != VMX_RESULT::SUCCESS)
        {
            LOG_ERROR("Failed to execute the __vmx_vmclear() intrinsic.");
            return Optional<Vcpu>();
        }

        if (__vmx_vmptrld(&vcpu.m_vmcs.value().PhysicalAddress()) != VMX_RESULT::SUCCESS)
        {
            LOG_ERROR("Failed to execute the __vmx_vmptrld() intrinsic.");
            return Optional<Vcpu>();
        }

        vcpu.m_msrBitmap = ContiguousMemory::allocate(PAGE_SIZE);
        if (!vcpu.m_msrBitmap.has())
        {
            LOG_ERROR("Failed to allocate contiguous memory for the MSR Bitmap.");
            return Optional<Vcpu>();
        }

        vcpu.m_hypervisorStack = PoolBuffer::allocate(
            HYPERVISOR_CONFIG::STACK_SIZE, POOL_FLAG_NON_PAGED, POOL_TAGS::STACK);
        if (!vcpu.m_hypervisorStack.has())
        {
            LOG_ERROR("Failed to allocate host stack.");
            return Optional<Vcpu>();
        }

        SYSTEM_DESCRIPTOR_TABLE_REGISTER guestGdtr = { 0 };
        AsmGetGdtr(&guestGdtr);

        vcpu.m_hostGdt = HostGdt::allocate(
            guestGdtr, AsmGetTr(), GDT_CONSTANTS::VMX_HOST_TR_LIMIT);

        if (!vcpu.m_hostGdt.has())
        {
            LOG_ERROR("Failed to allocate host GDT on core %lu.", processorIndex);
            return Optional<Vcpu>();
        }

        LOG_INFO("VCPU %lu successfully initialized.", processorIndex);

        // setting up the VMCS, then vmlaunch
        if (!AsmVirtualize(&vcpu))
        {
            UINT64 vmInstructionError = VmcsRead(VMCS_FIELDS::VM_INSTRUCTION_ERROR);
            LOG_ERROR("Failed to virtualize on core %lu. VM_INSTRUCTION_ERROR=%llu",
                      processorIndex, vmInstructionError);
            return Optional<Vcpu>();
        }
        vcpu.m_isLaunched = true;

        return Optional<Vcpu>(static_cast<Vcpu&&>(vcpu));
    }

    static bool AdjustControlValue(ULONG requestedValue, UINT64 msrValue, ULONG* outAdjustedValue)
    {
        LARGE_INTEGER msr = { 0 };
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

    static SEGMENT_INFO
    GetSegmentInfo(SEGMENT_SELECTOR selector, UINT64 gdtBase)
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

            segmentInfo.Base |= (static_cast<UINT64>(sysDescriptor->BaseUpper32) << BITS_32::HIGH_SHIFT);
        }

        segmentInfo.Limit = static_cast<UINT32>(
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
                ReadControlCapabilityMsr(IA32_VMX_MSR::TRUE_PINBASED_CTLS, IA32_VMX_MSR::PINBASED_CTLS),
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
                ReadControlCapabilityMsr(IA32_VMX_MSR::TRUE_PROCBASED_CTLS, IA32_VMX_MSR::PROCBASED_CTLS),
                &primaryControls))
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
                __readmsr(static_cast<ULONG>(IA32_VMX_MSR::PROCBASED_CTLS2)), &secondaryControls))
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
                ReadControlCapabilityMsr(IA32_VMX_MSR::TRUE_EXIT_CTLS, IA32_VMX_MSR::EXIT_CTLS),
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
                ReadControlCapabilityMsr(IA32_VMX_MSR::TRUE_ENTRY_CTLS, IA32_VMX_MSR::ENTRY_CTLS),
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
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_CR3, m_hostCr3);
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

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_FS_BASE, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::FS_BASE)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_GS_BASE, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::GS_BASE)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IA32_SYSENTER_CS, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::SYSENTER_CS)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IA32_SYSENTER_ESP, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::SYSENTER_ESP)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IA32_SYSENTER_EIP, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::SYSENTER_EIP)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SYSENTER_CS, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::SYSENTER_CS)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SYSENTER_ESP, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::SYSENTER_ESP)));
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_SYSENTER_EIP, __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::SYSENTER_EIP)));

        UINT8* stackTop = static_cast<UINT8*>(m_hypervisorStack.value().Pointer()) + HYPERVISOR_CONFIG::STACK_SIZE;

        // using some of the host stack for our own storage
        UINT64 stackBelowContext = reinterpret_cast<UINT64>(stackTop) - sizeof(HOST_STACK_CONTEXT);
        // we must align the host stack - otherwise our alignment calculations in the asm code will not work properly
        stackBelowContext &= ~(HOST_STACK::ALIGNMENT - 1ull);

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_RSP, stackBelowContext);

        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_RIP, reinterpret_cast<UINT64>(AsmVmExitHandler));

        // setting up guest segment registers

        SYSTEM_DESCRIPTOR_TABLE_REGISTER gdtr = { 0 };
        AsmGetGdtr(&gdtr);
        UINT64 gdtBase = gdtr.Base;

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
                        __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::FS_BASE)));

        SEGMENT_INFO gsInfo = GetSegmentInfo(gs, gdtBase);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_SELECTOR, gs.All);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_LIMIT, gsInfo.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_ACCESS_RIGHTS, gsInfo.AccessRights);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GS_BASE,
                        __readmsr(static_cast<ULONG>(IA32_SYSTEM_MSR::GS_BASE)));

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GDTR_BASE, gdtr.Base);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_GDTR_LIMIT, gdtr.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_GDTR_BASE, m_hostGdt.value().Base());
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_TR_BASE, trInfo.Base);

        // setting up the IDT

        SYSTEM_DESCRIPTOR_TABLE_REGISTER idtr = { 0 };
        AsmGetIdtr(&idtr);

        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_IDTR_BASE, idtr.Base);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_IDTR_LIMIT, idtr.Limit);
        VMCS_WRITE_SAFE(VMCS_FIELDS::HOST_IDTR_BASE, idtr.Base);

        // setting up fields that need to be zeroed out to be safe (we don't want to assume
        // we receive everything zeroed out in advance)
        VMCS_WRITE_SAFE(VMCS_FIELDS::EXCEPTION_BITMAP, 0ull);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_ACTIVITY_STATE, 0ull);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_INTERRUPTIBILITY_STATE, 0ull);
        VMCS_WRITE_SAFE(VMCS_FIELDS::GUEST_PENDING_DEBUG_EXCEPTIONS, 0ull);
        VMCS_WRITE_SAFE(VMCS_FIELDS::VM_ENTRY_INTERRUPTION_INFO, 0ull);
        VMCS_WRITE_SAFE(VMCS_FIELDS::CR3_TARGET_COUNT, 0ull);

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
