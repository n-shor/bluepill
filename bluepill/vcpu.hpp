#pragma once

#include "contiguousMemory.hpp"
#include "structs.h"
#include "utils.hpp"
#include <intrin.h>
#include <ntddk.h>

class Vcpu
{
private:
    ULONG m_processorIndex;
    Optional<ContiguousMemory> m_vmxon;
    Optional<ContiguousMemory> m_vmcs;

    void EnableVmx()
    {
        const unsigned long long oldCr4 = __readcr4();

        static constexpr size_t CR4_VMXE_BIT = 1ull << 13;
        __writecr4(oldCr4 | CR4_VMXE_BIT);
    }

    void DisableVmx()
    {
        const unsigned long long oldCr4 = __readcr4();

        static constexpr size_t CR4_VMXE_BIT = 1ull << 13;
        __writecr4(oldCr4 & (~CR4_VMXE_BIT));
    }

public:
    Vcpu() = default;
    ~Vcpu() = default;

    // needs to run on the specific core this VCPU object is assigned to
    bool Initialize(const ULONG processorIndex, const EPT_POINTER eptPointer)
    {
        m_processorIndex = processorIndex;

        IA32_FEATURE_CONTROL_MSR featureControl = { 0 };
        static constexpr size_t MSR_IA32_FEATURE_CONTROL = 0x3A;
        featureControl.All = __readmsr(MSR_IA32_FEATURE_CONTROL);

        if (featureControl.Fields.Lock == FALSE)
        {
            featureControl.Fields.Lock = TRUE;
            featureControl.Fields.EnableVMXON = TRUE;

            __writemsr(MSR_IA32_FEATURE_CONTROL, featureControl.All);
        }
        else if (featureControl.Fields.EnableVMXON == FALSE)
        {
            DbgPrint("[-] ERROR: VMX locked off by BIOS on core %lu.\n", m_processorIndex);
            return false;
        }

        EnableVmx();

        m_vmxon = ContiguousMemory::allocate(PAGE_SIZE);
        if (!m_vmxon.has())
        {
            DbgPrint("[-] ERROR: Failed to allocate contiguous memory for VMXON Region.\n");

            DisableVmx();

            return false;
        }

        IA32_VMX_BASIC_MSR vmxBasic = { 0 };

        static constexpr size_t MSR_IA32_VMX_BASIC = 0x480;
        vmxBasic.All = __readmsr(MSR_IA32_VMX_BASIC);
        ULONG revisionId = static_cast<ULONG>(vmxBasic.All);

        // writing the revision ID into the vmxon memory, this is necessary to ensure everything is compatible
        *(reinterpret_cast<ULONG*>(m_vmxon.value().VirtualAddress())) = revisionId;

        if (__vmx_on(&m_vmxon.value().PhysicalAddress()) != 0)
        {
            DbgPrint("[-] ERROR: Failed to execute the __vmx_on() intrinsic.\n");
            DisableVmx();

            return false;
        }

        m_vmcs = ContiguousMemory::allocate(PAGE_SIZE);
        if (!m_vmcs.has())
        {
            DbgPrint("[-] ERROR: Failed to allocate contiguous memory for VMCS Region.\n");

            __vmx_off();
            DisableVmx();

            return false;
        }

        *(reinterpret_cast<ULONG*>(m_vmcs.value().VirtualAddress())) = revisionId;

        if (__vmx_vmclear(&m_vmcs.value().PhysicalAddress()) != 0)
        {
            DbgPrint("[-] ERROR: Failed to execute the __vmx_vmclear() intrinsic.\n");

            __vmx_off();
            DisableVmx();

            return false;
        }

        if (__vmx_vmptrld(&m_vmcs.value().PhysicalAddress()) != 0)
        {
            DbgPrint("[-] ERROR: Failed to execute the __vmx_vmptrld() intrinsic.\n");

            __vmx_off();
            DisableVmx();

            return false;
        }

        // setting up the VMCS

        static constexpr UINT64 VMCS_CTRL_EPT_POINTER = 0x201A;
        if (__vmx_vmwrite(VMCS_CTRL_EPT_POINTER, eptPointer.All) != 0)
        {
            DbgPrint("[-] ERROR: Failed to write EPT pointer to VMCS on core %lu.\n", m_processorIndex);

            __vmx_off();
            DisableVmx();

            return false;
        }

        DbgPrint("[+] VCPU %lu successfully initialized.\n", m_processorIndex);
        return true;
    }

    void Teardown()
    {
        __vmx_off();

        m_vmxon.clear();
        m_vmcs.clear();

        DisableVmx();

        DbgPrint("[*] VCPU %lu successfully powered down and memory freed.\n", m_processorIndex);
    }
};
