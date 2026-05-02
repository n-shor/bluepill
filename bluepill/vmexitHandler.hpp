#pragma once

#include "constants.h"
#include "structs.h"
#include <intrin.h>

#define VMCS_READ(Field) \
    ([]() -> size_t { size_t result = 0; __vmx_vmread(static_cast<size_t>(Field), &result); return result; })()

extern "C" void CppVmExitDispatcher(GUEST_REGISTERS* GuestRegs)
{
    size_t fullExitReason = VMCS_READ(VMCS_FIELDS::VM_EXIT_REASON);

    if (fullExitReason & VMX_ENTRY_FAILURE_FLAG)
    {
        size_t failedReason = fullExitReason & VMX_BASIC_EXIT_REASON_MASK;
        LOG_ERROR("VM-Entry failed! Hardware rejection code: %llu", failedReason);

        return;
    }

    VMEXIT_REASON exitReason = static_cast<VMEXIT_REASON>(fullExitReason & VMX_BASIC_EXIT_REASON_MASK);
    size_t instructionLength = VMCS_READ(VMCS_FIELDS::VM_EXIT_INSTRUCTION_LEN);
    size_t guestRip = VMCS_READ(VMCS_FIELDS::GUEST_RIP);

    bool advanceRip = true;

    switch (static_cast<VMEXIT_REASON>(exitReason))
    {
    case VMEXIT_REASON::CPUID_EXIT:
    {
        // LOG_INFO("Intercepted CPUID execution at RIP: 0x%llX", guestRip);

        int cpuInfo[4];
        __cpuidex(cpuInfo, static_cast<int>(GuestRegs->Rax), static_cast<int>(GuestRegs->Rcx));

        // if windows is checking standard CPU features (Leaf 1)
        if (GuestRegs->Rax == 1)
        {
            // clear bit 31 in ECX to hide the hypervisor from the OS
            cpuInfo[2] &= ~(1 << 31);
        }

        GuestRegs->Rax = static_cast<ULONG32>(cpuInfo[0]);
        GuestRegs->Rbx = static_cast<ULONG32>(cpuInfo[1]);
        GuestRegs->Rcx = static_cast<ULONG32>(cpuInfo[2]);
        GuestRegs->Rdx = static_cast<ULONG32>(cpuInfo[3]);

        break;
    }
    case VMEXIT_REASON::RDMSR:
    {
        GuestRegs->Rax = 0;
        GuestRegs->Rdx = 0;

        break;
    }
    case VMEXIT_REASON::WRMSR:
    {
        break;
    }
    case VMEXIT_REASON::IO_INSTRUCTION:
    {
        // LOG_INFO("Intercepted IO_INSTRUCTION vmexit at RIP: 0x%llX", guestRip);

        break;
    }
    case VMEXIT_REASON::EPT_VIOLATION:
    {
        // size_t faultingGpa = VMCS_READ(VMCS_FIELDS::GUEST_PHYSICAL_ADDRESS);
        // LOG_ERROR("Unexpected EPT Violation at GPA: 0x%llX", faultingGpa);

        // we do not have dynamic mapping enabled because our EPT is premapped.
        // if we reach here, windows tried to access memory over 512GB, which should not happen.
        __debugbreak();

        // if we somehow resolve it, we flush the cache and retry the instruction
        /*
        INVEPT_DESCRIPTOR descriptor = { g_Ept->GetEptPointer().All, 0 };
        AsmInveptAllContexts(&descriptor);
        */

        // we need the guest to retry the instruction
        advanceRip = false;

        break;
    }
    case VMEXIT_REASON::RDTSC:
    {
        // transparent passthrough - just let the guest see real TSC
        UINT64 tsc = __rdtsc();
        GuestRegs->Rax = tsc & 0xFFFFFFFF;
        GuestRegs->Rdx = tsc >> 32;
        break;
    }
    default:
    {
        // LOG_INFO("Unhandled VM-Exit. Reason: %llu", exitReason);
        __debugbreak();

        break;
    }
    }

    // we advance RIP, otherwise the guest will execute the same instruction after we run vmresume
    if (advanceRip)
    {
        __vmx_vmwrite(static_cast<size_t>(VMCS_FIELDS::GUEST_RIP), guestRip + instructionLength);
    }

    // after we return from this function, the assembly code will run vmresume
}
