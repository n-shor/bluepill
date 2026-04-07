#pragma once

#include "ept.hpp"
#include "vcpu.hpp"
#include <intrin.h>
#include <ntddk.h>

// change pool tag later to 'erhT' later to avoid easy detection in memory
static constexpr ULONG POOL_TAG = 'llip';

class Hypervisor
{
private:
    Vcpu* m_vcpus = nullptr;
    ULONG m_processorCount = 0;
    VmmEpt m_ept;

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

    bool IsEptSupportedGlobally()
    {
        IA32_VMX_EPT_VPID_CAP_MSR eptVpidCap = { 0 };
        static constexpr size_t MSR_IA32_VMX_EPT_VPID_CAP = 0x48C;

        eptVpidCap.All = __readmsr(MSR_IA32_VMX_EPT_VPID_CAP);

        return eptVpidCap.Fields.SupportPageWalkLength4 &&
               eptVpidCap.Fields.SupportWriteBackMemoryType &&
               eptVpidCap.Fields.SupportPde2mbPages;
    }

public:
    Hypervisor() = default;

    ~Hypervisor()
    {
        Stop();
    }

    bool Start()
    {
        if (!IsVmxSupportedGlobally())
        {
            DbgPrint("[-] ERROR: Intel VT-x is NOT supported by the CPU.\n");
            return false;
        }

        if (!IsEptSupportedGlobally())
        {
            DbgPrint("[-] ERROR: EPT is NOT supported by the CPU.\n");
            return false;
        }

        if (!m_ept.Initialize())
        {
            DbgPrint("[-] ERROR: Failed to initialize EPT.\n");
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
            bool success = m_vcpus[i].Initialize(i, m_ept.GetEptPointer());
            KeRevertToUserAffinityThreadEx(oldAffinity);

            // if a core fails, for now we abort the entire driver startup.
            // maybe one day we work on a way to work around this for a more robust rootkit
            if (!success)
            {
                DbgPrint("[-] ERROR: Initialization failed on core %lu. Aborting.\n", i);

                // rollback process to ensure we don't destroy anything:

                for (ULONG j = 0; j < i; ++j)
                {
                    // only works if we have fewer than 64 cores, which is a safe assumption for now.
                    // if we had more than 64 cores, we would need to use processor groups (KeSetSystemGroupAffinityThread)
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

        m_ept.Teardown();

        DbgPrint("[*] Hypervisor successfully stopped and memory freed.\n");
    }
};
