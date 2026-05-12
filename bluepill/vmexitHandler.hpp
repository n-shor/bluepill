#pragma once

#include "constants.hpp"
#include "structs.hpp"
#include <intrin.h>

#define VMCS_READ(Field) \
    ([]() -> UINT64 { UINT64 result = 0; __vmx_vmread(static_cast<UINT64>(Field), &result); return result; })()

extern "C" volatile bool g_ShutdownThisCpu;

extern "C" volatile UINT64 g_ShutdownResumeRip;
extern "C" volatile UINT64 g_ShutdownGuestRsp;
extern "C" volatile UINT64 g_ShutdownGuestRflags;

extern "C" void CppVmExitDispatcher(GUEST_REGISTERS* GuestRegs)
{
    UINT64 guestRip = VMCS_READ(VMCS_FIELDS::GUEST_RIP);
    UINT64 fullExitReason = VMCS_READ(VMCS_FIELDS::VM_EXIT_REASON);

    if (fullExitReason & VMX_ENTRY_FAILURE_FLAG)
    {
        UINT64 failedReason = fullExitReason & VMX_BASIC_EXIT_REASON_MASK;
        LOG_ERROR("VM-Entry failed! Hardware rejection code: %llu", failedReason);

        _enable(); // reenabling interrupts so KeBugCheckEx can work
        KeBugCheckEx(
            0xDEAD0000,
            failedReason, // parameter 1: the specific VM-entry failure reason
            guestRip,     // parameter 2: where the guest was when it failed
            0, 0);
    }

    VMEXIT_REASON exitReason = static_cast<VMEXIT_REASON>(fullExitReason & VMX_BASIC_EXIT_REASON_MASK);
    UINT64 instructionLength = VMCS_READ(VMCS_FIELDS::VM_EXIT_INSTRUCTION_LEN);

    bool advanceRip = true;

    switch (static_cast<VMEXIT_REASON>(exitReason))
    {
    case VMEXIT_REASON::CPUID_EXIT:
    {
        int cpuInfo[CPUID_REGISTER::COUNT];
        __cpuidex(cpuInfo, static_cast<int>(GuestRegs->Rax), static_cast<int>(GuestRegs->Rcx));

        if (GuestRegs->Rax == 1)
        {
            // clearing bit 31 in ECX to hide the hypervisor from the operating system
            cpuInfo[CPUID_REGISTER::ECX] &= ~(CPUID_FEATURES::HYPERVISOR_PRESENT);
        }

        GuestRegs->Rax = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::EAX]);
        GuestRegs->Rbx = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::EBX]);
        GuestRegs->Rcx = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::ECX]);
        GuestRegs->Rdx = static_cast<ULONG32>(cpuInfo[CPUID_REGISTER::EDX]);

        break;
    }
    // we handle MSR access for the MSRs that are not covered by the zeroed out bitmap ranges
    case VMEXIT_REASON::RDMSR:
    {
        const ULONG32 msrIndex = static_cast<ULONG32>(GuestRegs->Rcx);
        const ULONG64 value = __readmsr(msrIndex);

        GuestRegs->Rax = value & BITS_32::LOW_MASK;
        GuestRegs->Rdx = value >> BITS_32::HIGH_SHIFT;
        break;
    }
    case VMEXIT_REASON::WRMSR:
    {
        const ULONG32 msrIndex = static_cast<ULONG32>(GuestRegs->Rcx);
        const ULONG64 value = (GuestRegs->Rax & BITS_32::LOW_MASK) | (GuestRegs->Rdx << BITS_32::HIGH_SHIFT);

        __writemsr(msrIndex, value);
        break;
    }
    case VMEXIT_REASON::EPT_VIOLATION:
    {
        advanceRip = false; // we want the guest to try again

        UINT64 faultingGpa = VMCS_READ(VMCS_FIELDS::GUEST_PHYSICAL_ADDRESS);
        UINT64 exitQualification = VMCS_READ(VMCS_FIELDS::VM_EXIT_QUALIFICATION);

        _enable(); // reenabling interrupts so KeBugCheckEx can work
        KeBugCheckEx(
            0xDEAD001,
            faultingGpa,
            guestRip,
            exitQualification, 0);

        break;
    }
    case VMEXIT_REASON::XSETBV:
    {
        const ULONG32 xcrIndex = static_cast<ULONG32>(GuestRegs->Rcx);
        const ULONG64 value = (GuestRegs->Rax & BITS_32::LOW_MASK) | (GuestRegs->Rdx << BITS_32::HIGH_SHIFT);

        _xsetbv(xcrIndex, value);
        break;
    }
    case VMEXIT_REASON::VMCALL:
    {
        if (GuestRegs->Rcx == HYPERVISOR_CONFIG::SHUTDOWN_HYPERCALL)
        {
            // gathering everything we need to return to the guest before vmxoff
            g_ShutdownResumeRip = guestRip + instructionLength;
            g_ShutdownGuestRsp = VMCS_READ(VMCS_FIELDS::GUEST_RSP);
            g_ShutdownGuestRflags = VMCS_READ(VMCS_FIELDS::GUEST_RFLAGS);

            // we need to write the data globals before the shutdown flag so the asm
            // code never sees g_ShutdownThisCpu == true with stale resume info
            _WriteBarrier();

            g_ShutdownThisCpu = true;

            advanceRip = false; // we set RIP manually via the resume path
        }
        else
        {
            LOG_ERROR("Unknown VMCALL code: 0x%llX", GuestRegs->Rcx);
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
