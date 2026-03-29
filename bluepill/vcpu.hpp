#pragma once
#include <ntddk.h>
#include <intrin.h>
#include "structs.h"

class Vcpu {
private:
    ULONG m_processorIndex;
    PVOID m_vmxonVirtualAddress;
    unsigned long long m_vmxonPhysicalAddress;
    PVOID m_vmcsVirtualAddress;
    unsigned long long m_vmcsPhysicalAddress;

    void EnableVmx() {
        const unsigned long long oldCr4 = __readcr4();

        static constexpr size_t CR4_VMXE_BIT = 1ull << 13;
        __writecr4(oldCr4 | CR4_VMXE_BIT);
    }

    void DisableVmx() {
        const unsigned long long oldCr4 = __readcr4();

        static constexpr size_t CR4_VMXE_BIT = 1ull << 13;
        __writecr4(oldCr4 & (~CR4_VMXE_BIT));
    }

public:
    Vcpu() = default;
    ~Vcpu() = default;

    // needs to run on the specific core this VCPU object is assigned to
    bool Initialize(ULONG processorIndex) {
        m_processorIndex = processorIndex;

        IA32_FEATURE_CONTROL_MSR featureControl = { 0 };
        static constexpr size_t MSR_IA32_FEATURE_CONTROL = 0x3A;
        featureControl.All = __readmsr(MSR_IA32_FEATURE_CONTROL);

        if (featureControl.Fields.Lock == FALSE) {
            featureControl.Fields.Lock = TRUE;
            featureControl.Fields.EnableVMXON = TRUE;

            __writemsr(MSR_IA32_FEATURE_CONTROL, featureControl.All);
        }
        else if (featureControl.Fields.EnableVMXON == FALSE) {
            DbgPrint("[-] ERROR: VMX locked off by BIOS on core %lu.\n", m_processorIndex);
            return false;
        }

        EnableVmx();
        
        PHYSICAL_ADDRESS maximumAddress;
        maximumAddress.QuadPart = MAXULONG64;
        m_vmxonVirtualAddress = MmAllocateContiguousMemory(PAGE_SIZE, maximumAddress);
        if (m_vmxonVirtualAddress == nullptr) {
            DbgPrint("[-] ERROR: Failed to allocate contiguous memory for VMXON Region.\n");

            DisableVmx();

            return false;
        }

        RtlSecureZeroMemory(m_vmxonVirtualAddress, PAGE_SIZE);

        m_vmxonPhysicalAddress = MmGetPhysicalAddress(m_vmxonVirtualAddress).QuadPart;

        IA32_VMX_BASIC_MSR vmxBasic = { 0 };

        static constexpr size_t MSR_IA32_VMX_BASIC = 0x480;
        vmxBasic.All = __readmsr(MSR_IA32_VMX_BASIC);

        // writing the revision ID into the vmxon memory, this is necessary to ensure everything is compatible
        ULONG revisionId = static_cast<ULONG>(vmxBasic.All);
        *(reinterpret_cast<ULONG*>(m_vmxonVirtualAddress)) = revisionId;

        if (__vmx_on(&m_vmxonPhysicalAddress) != 0)
        {
            DbgPrint("[-] ERROR: Failed to execute the __vmx_on() intrinsic.\n");

            DisableVmx();

            MmFreeContiguousMemory(m_vmxonVirtualAddress);
            m_vmxonVirtualAddress = nullptr;
            m_vmxonPhysicalAddress = 0;

            return false;
        }

        maximumAddress.QuadPart = MAXULONG64;
        m_vmcsVirtualAddress = MmAllocateContiguousMemory(PAGE_SIZE, maximumAddress);
        if (m_vmcsVirtualAddress == nullptr) {
            DbgPrint("[-] ERROR: Failed to allocate contiguous memory for VMCS Region.\n");

            __vmx_off();
            DisableVmx();

            MmFreeContiguousMemory(m_vmxonVirtualAddress);
            m_vmxonVirtualAddress = nullptr;
            m_vmxonPhysicalAddress = 0;

            return false;
        }

        RtlSecureZeroMemory(m_vmcsVirtualAddress, PAGE_SIZE);

        m_vmcsPhysicalAddress = MmGetPhysicalAddress(m_vmcsVirtualAddress).QuadPart;

        *(reinterpret_cast<ULONG*>(m_vmcsVirtualAddress)) = revisionId;

        if (__vmx_vmclear(&m_vmcsPhysicalAddress) != 0)
        {
            DbgPrint("[-] ERROR: Failed to execute the __vmx_vmclear() intrinsic.\n");

            __vmx_off();
            DisableVmx();

            MmFreeContiguousMemory(m_vmxonVirtualAddress);
            m_vmxonVirtualAddress = nullptr;
            m_vmxonPhysicalAddress = 0;

            MmFreeContiguousMemory(m_vmcsVirtualAddress);
            m_vmcsVirtualAddress = nullptr;
            m_vmcsPhysicalAddress = 0;

            return false;
        }

        if (__vmx_vmptrld(&m_vmcsPhysicalAddress) != 0)
        {
            DbgPrint("[-] ERROR: Failed to execute the __vmx_vmptrld() intrinsic.\n");

            __vmx_off();
            DisableVmx();

            MmFreeContiguousMemory(m_vmxonVirtualAddress);
            m_vmxonVirtualAddress = nullptr;
            m_vmxonPhysicalAddress = 0;

            MmFreeContiguousMemory(m_vmcsVirtualAddress);
            m_vmcsVirtualAddress = nullptr;
            m_vmcsPhysicalAddress = 0;

            return false;
        }

        DbgPrint("[+] VCPU %lu successfully initialized.\n", m_processorIndex);
        return true;
    }

    void Teardown() {
        __vmx_off();
        DisableVmx();

        if (m_vmxonVirtualAddress != nullptr) {
            MmFreeContiguousMemory(m_vmxonVirtualAddress);
            m_vmxonVirtualAddress = nullptr;
            m_vmxonPhysicalAddress = 0;
        }
        if (m_vmcsVirtualAddress != nullptr)
        {
            MmFreeContiguousMemory(m_vmcsVirtualAddress);
            m_vmcsVirtualAddress = nullptr;
            m_vmcsPhysicalAddress = 0;
        }

        DbgPrint("[*] VCPU %lu successfully powered down and memory freed.\n", m_processorIndex);
    }
};
