#pragma once
#include <ntddk.h>
#include <intrin.h>
#include "vcpu.hpp"

// change pool tag later to 'erhT' later to avoid easy detection in memory
static constexpr ULONG POOL_TAG = 'llip';

class Hypervisor
{
private:
    Vcpu* m_vcpus = nullptr;
    ULONG m_processorCount = 0;

    bool IsVmxSupportedGlobally()
    {
        CPUID cpuInfo = { 0 };
        static constexpr int LEAF_1 = 1;
        __cpuid(reinterpret_cast<int*>(&cpuInfo), LEAF_1);

        static constexpr size_t CPUID_VMX_BIT = 1ull << 5;
        if ((cpuInfo.ecx & CPUID_VMX_BIT) == 0)
        {
            return false;
        }

        return true;
    }

public:
    Hypervisor() = default;

    bool Start()
    {
        if (!IsVmxSupportedGlobally())
        {
            DbgPrint("[-] ERROR: Intel VT-x is NOT supported by the CPU.\n");
            return false;
        }
        
        m_processorCount = KeQueryActiveProcessorCount(NULL);

        size_t vcpuArraySize = sizeof(Vcpu) * m_processorCount;
        m_vcpus = static_cast<Vcpu*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, vcpuArraySize, POOL_TAG));
        if (m_vcpus == nullptr)
        {
            DbgPrint("[-] ERROR: Failed to allocate virtual CPU array.\n");
            return false;
        }

        for (ULONG i = 0; i < m_processorCount; ++i)
        {
            KAFFINITY oldAffinity = KeSetSystemAffinityThreadEx(1ull << i);
            bool success = m_vcpus[i].Initialize(i);
            KeRevertToUserAffinityThreadEx(oldAffinity);

            // if a core fails, for now we abort the entire driver startup.
            // maybe one day we work on a way to work around this for a more robust rootkit
            if (!success)
            {
                DbgPrint("[-] ERROR: Initialization failed on core %lu. Aborting.\n", i);

                // rollback process to ensure we don't destroy anything:

                for (ULONG j = 0; j < i; ++j)
                {
                    KAFFINITY rollbackAffinity = KeSetSystemAffinityThreadEx(1ull << j);
                    m_vcpus[j].Teardown();
                    KeRevertToUserAffinityThreadEx(rollbackAffinity);
                }

                ExFreePoolWithTag(m_vcpus, POOL_TAG);
                m_vcpus = nullptr;

                return false;
            }
        }

        DbgPrint("[+] SUCCESS: Hypervisor started on all cores!\n");
        return true;
    }

    void Stop()
    {
        if (m_vcpus != nullptr)
        {
            for (ULONG i = 0; i < m_processorCount; ++i)
            {
                KAFFINITY oldAffinity = KeSetSystemAffinityThreadEx(1ull << i);
                m_vcpus[i].Teardown();
                KeRevertToUserAffinityThreadEx(oldAffinity);
            }

            ExFreePoolWithTag(m_vcpus, POOL_TAG);
            m_vcpus = nullptr;
        }

        DbgPrint("[*] Hypervisor stopped safely.\n");
    }
};
