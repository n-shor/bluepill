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

extern "C" __declspec(noreturn) void HandleVmresumeFailure()
{
    UINT64 vmInstructionError = VmcsRead(VMCS_FIELDS::VM_INSTRUCTION_ERROR);
    UINT64 guestRip = VmcsRead(VMCS_FIELDS::GUEST_RIP);

    LOG_ERROR("VMRESUME failed! VM_INSTRUCTION_ERROR=%llu, guest RIP=0x%llX",
              vmInstructionError, guestRip);
#if DBG
    _enable();
    KeBugCheckEx(BUGCHECK_CODES::VMRESUME_FAILURE,
                 vmInstructionError, guestRip, 0, 0);
#else
    while (true)
    {
        _mm_pause();
    }
#endif
}

extern "C" __declspec(noreturn) void HandleVmxoffFailure()
{
    UINT64 vmInstructionError = VmcsRead(VMCS_FIELDS::VM_INSTRUCTION_ERROR);

    LOG_ERROR("VMXOFF failed! VM_INSTRUCTION_ERROR=%llu", vmInstructionError);

#if DBG
    _enable();
    KeBugCheckEx(BUGCHECK_CODES::VMXOFF_FAILURE,
                 vmInstructionError, 0, 0, 0);
#else
    while (true)
    {
        _mm_pause();
    }
#endif
}

extern "C" bool CppVmExitDispatcher(GUEST_REGISTERS* GuestRegs)
{
    UINT64 guestRip = VmcsRead(VMCS_FIELDS::GUEST_RIP);
    UINT64 fullExitReason = VmcsRead(VMCS_FIELDS::VM_EXIT_REASON);

    if (fullExitReason & VMEXIT_REASON_MASKS::ENTRY_FAILURE_FLAG)
    {
        UINT64 failedReason = fullExitReason & VMEXIT_REASON_MASKS::BASIC_REASON;
        LOG_ERROR("VM-Entry failed! Hardware rejection code: %llu", failedReason);
#if DBG
        _enable(); // reenabling interrupts so KeBugCheckEx can work
        KeBugCheckEx(BUGCHECK_CODES::VM_ENTRY_FAILURE,
                     failedReason, guestRip, 0, 0);
#else
        while (true)
        {
            _mm_pause();
        }
#endif
    }

    VMEXIT_REASON exitReason = static_cast<VMEXIT_REASON>(
        fullExitReason & VMEXIT_REASON_MASKS::BASIC_REASON);
    UINT64 instructionLength = VmcsRead(VMCS_FIELDS::VM_EXIT_INSTRUCTION_LEN);

    bool shutdownRequested = false;
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

        LOG_ERROR("Unexpected EPT violation. GPA=0x%llX RIP=0x%llX Qualification=0x%llX",
                  faultingGpa, guestRip, exitQualification);

#if DBG
        _enable();
        KeBugCheckEx(BUGCHECK_CODES::EPT_VIOLATION,
                     faultingGpa, guestRip, exitQualification, 0);
#endif

        break;
    }
    case VMEXIT_REASON::VMCALL:
    {
        if (GuestRegs->Rcx == HYPERVISOR_CONFIG::SHUTDOWN_HYPERCALL)
        {
            // we advance RIP normally here so the assembly code can read the next RIP from the VMCS
            shutdownRequested = true;
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
        LOG_ERROR("Unhandled VM-Exit. Reason: %llu", static_cast<UINT64>(exitReason));
        advanceRip = false;

#if DBG
        _enable();
        KeBugCheckEx(BUGCHECK_CODES::UNHANDLED_EXIT,
                     static_cast<ULONG64>(exitReason), guestRip, 0, 0);
#endif

        break;
    }
    }

    // we advance RIP, otherwise the guest will execute the same instruction after we run vmresume
    if (advanceRip)
    {
        __vmx_vmwrite(static_cast<UINT64>(VMCS_FIELDS::GUEST_RIP),
                      guestRip + instructionLength);
    }

    return shutdownRequested;
}
