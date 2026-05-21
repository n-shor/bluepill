#pragma once

#include "constants.hpp"
#include "structs.hpp"
#include <intrin.h>

inline UINT64 VmcsRead(VMCS_FIELDS field)
{
    UINT64 result = 0;
    UCHAR status = __vmx_vmread(static_cast<UINT64>(field), &result);

    NT_ASSERT(status == VMX_RESULT::SUCCESS);

    return result;
}

inline UINT32 GetGuestCpl()
{
    // CPL resides in bits 5-6 of the SS access rights field. usually we would access
    // the low 2 bits of CS, but intel defined SS to be consistent while CS could
    // sometimes cause issues
    UINT64 ssAccessRights = VmcsRead(VMCS_FIELDS::GUEST_SS_ACCESS_RIGHTS);
    return static_cast<UINT32>(
        (ssAccessRights >> SEGMENT_SHIFTS::AR_DPL) & SEGMENT_SHIFTS::AR_DPL_MASK);
}

// injects an exception for delivery on the next VMENTRY. the caller is
// responsible for NOT advancing the guest RIP, because the exception
// replaces the faulting instruction
inline void InjectException(UINT32 vector, bool hasErrorCode, UINT32 errorCode)
{
    UINT32 info = VM_ENTRY_INTERRUPTION::VALID |
                  VM_ENTRY_INTERRUPTION::TYPE_HARDWARE_EXCEPTION |
                  vector;

    if (hasErrorCode)
    {
        info |= VM_ENTRY_INTERRUPTION::DELIVER_ERROR_CODE;

        UCHAR result = __vmx_vmwrite(
            static_cast<UINT64>(VMCS_FIELDS::VM_ENTRY_EXCEPTION_ERROR_CODE), errorCode);
        NT_ASSERT(result == VMX_RESULT::SUCCESS);
    }

    UCHAR result = __vmx_vmwrite(
        static_cast<UINT64>(VMCS_FIELDS::VM_ENTRY_INTERRUPTION_INFO), info);
    NT_ASSERT(result == VMX_RESULT::SUCCESS);
}

inline void InjectGpFault()
{
    InjectException(EXCEPTION_VECTORS::GP, true, EXCEPTION_ERROR_CODES::GP_NON_SEGMENT);
}

inline void InjectUdFault()
{
    InjectException(EXCEPTION_VECTORS::UD, false, NULL);
}

extern "C" volatile bool g_ShutdownThisCpu;

extern "C" volatile UINT64 g_ShutdownResumeRip;
extern "C" volatile UINT64 g_ShutdownGuestRsp;
extern "C" volatile UINT64 g_ShutdownGuestRflags;

extern "C" void CppVmExitDispatcher(GUEST_REGISTERS* GuestRegs)
{
    UINT64 guestRip = VmcsRead(VMCS_FIELDS::GUEST_RIP);
    UINT64 fullExitReason = VmcsRead(VMCS_FIELDS::VM_EXIT_REASON);

    if (fullExitReason & VMEXIT_REASON_MASKS::ENTRY_FAILURE_FLAG)
    {
        UINT64 failedReason = fullExitReason & VMEXIT_REASON_MASKS::BASIC_REASON;
        LOG_ERROR("VM-Entry failed! Hardware rejection code: %llu", failedReason);

        _enable(); // reenabling interrupts so KeBugCheckEx can work
        KeBugCheckEx(
            0xDEAD0000,
            failedReason, // parameter 1: the specific VM-entry failure reason
            guestRip,     // parameter 2: where the guest was when it failed
            0, 0);
    }

    VMEXIT_REASON exitReason = static_cast<VMEXIT_REASON>(
        fullExitReason & VMEXIT_REASON_MASKS::BASIC_REASON);
    UINT64 instructionLength = VmcsRead(VMCS_FIELDS::VM_EXIT_INSTRUCTION_LEN);

    bool advanceRip = true;

    switch (static_cast<VMEXIT_REASON>(exitReason))
    {
    case VMEXIT_REASON::CPUID_EXIT:
    {
        int cpuInfo[CPUID_REGISTER::COUNT];
        __cpuidex(cpuInfo, static_cast<int>(GuestRegs->Rax), static_cast<int>(GuestRegs->Rcx));

        if (GuestRegs->Rax == CPUID_LEAF::VERSION_AND_FEATURES)
        {
            // clearing bit 31 in ECX to hide the hypervisor from the operating system
            cpuInfo[CPUID_REGISTER::ECX] &= ~(CPUID_FEATURES::HYPERVISOR_PRESENT);
        }

        // CPUID zero extends results to 64 bits in long mode, so the ULONG32 cast
        // ensures we don't sign extend the int through to the registers' upper bits
        GuestRegs->Rax = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::EAX]);
        GuestRegs->Rbx = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::EBX]);
        GuestRegs->Rcx = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::ECX]);
        GuestRegs->Rdx = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::EDX]);

        break;
    }
    // we handle MSR access for the MSRs that are not covered by the zeroed out bitmap ranges
    case VMEXIT_REASON::RDMSR:
    {
        if (GetGuestCpl() != CPL::KERNEL)
        {
            InjectGpFault();
            advanceRip = false;
            break;
        }

        const ULONG32 msrIndex = static_cast<ULONG32>(GuestRegs->Rcx);
        const ULONG64 value = __readmsr(msrIndex);

        GuestRegs->Rax = value & BITS_32::LOW_MASK;
        GuestRegs->Rdx = value >> BITS_32::HIGH_SHIFT;
        break;
    }
    case VMEXIT_REASON::WRMSR:
    {
        if (GetGuestCpl() != CPL::KERNEL)
        {
            InjectGpFault();
            advanceRip = false;
            break;
        }

        const ULONG32 msrIndex = static_cast<ULONG32>(GuestRegs->Rcx);
        const ULONG64 value = (GuestRegs->Rax & BITS_32::LOW_MASK) | (GuestRegs->Rdx << BITS_32::HIGH_SHIFT);

        __writemsr(msrIndex, value);
        break;
    }
    case VMEXIT_REASON::XSETBV:
    {
        if (GetGuestCpl() != CPL::KERNEL)
        {
            InjectGpFault();
            advanceRip = false;
            break;
        }

        const ULONG32 xcrIndex = static_cast<ULONG32>(GuestRegs->Rcx);
        const ULONG64 value = (GuestRegs->Rax & BITS_32::LOW_MASK) | (GuestRegs->Rdx << BITS_32::HIGH_SHIFT);

        _xsetbv(xcrIndex, value);
        break;
    }
    case VMEXIT_REASON::EPT_VIOLATION:
    {
        advanceRip = false; // we want the guest to try again

        UINT64 faultingGpa = VmcsRead(VMCS_FIELDS::GUEST_PHYSICAL_ADDRESS);
        UINT64 exitQualification = VmcsRead(VMCS_FIELDS::VM_EXIT_QUALIFICATION);

        _enable(); // reenabling interrupts so KeBugCheckEx can work
        KeBugCheckEx(
            0xDEAD001,
            faultingGpa,
            guestRip,
            exitQualification, 0);

        break;
    }
    case VMEXIT_REASON::VMCALL:
    {
        if (GuestRegs->Rcx == HYPERVISOR_CONFIG::SHUTDOWN_HYPERCALL)
        {
            // gathering everything we need to return to the guest before vmxoff
            g_ShutdownResumeRip = guestRip + instructionLength;
            g_ShutdownGuestRsp = VmcsRead(VMCS_FIELDS::GUEST_RSP);
            g_ShutdownGuestRflags = VmcsRead(VMCS_FIELDS::GUEST_RFLAGS);

            // we need to write the data globals before the shutdown flag so the asm
            // code never sees g_ShutdownThisCpu == true with stale resume info
            _WriteBarrier();

            g_ShutdownThisCpu = true;

            advanceRip = false; // we set RIP manually via the resume path
        }
        else
        {
            // VMCALLs outside VMX root raise #UD on bare metal, so we mirror that for
            // any guest code that issues an unknown VMCALL
            InjectUdFault();
            advanceRip = false;
        }
        break;
    }
    default:
    {
        LOG_ERROR("Unhandled VM-Exit. Reason: %llu", exitReason);
        /*
        _enable(); // reenabling interrupts so KeBugCheckEx can work
        KeBugCheckEx(
            0xDEAD0002,
            static_cast<ULONG64>(exitReason),
            guestRip,
            0, 0);
        */
        break;
    }
    }

    // we advance RIP, otherwise the guest will execute the same instruction after we run vmresume
    if (advanceRip)
    {
        __vmx_vmwrite(static_cast<UINT64>(VMCS_FIELDS::GUEST_RIP), guestRip + instructionLength);
    }

    // after we return from this function, the assembly code will run vmresume
}
