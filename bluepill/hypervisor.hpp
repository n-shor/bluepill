#pragma once

#include "constants.hpp"
#include "ept.hpp"
#include "vcpu.hpp"
#include <intrin.h>
#include <ntddk.h>

class Hypervisor
{
private:
    Vcpu* m_vcpus = nullptr;
    ULONG m_processorCount = 0;
    VmmEpt m_ept;

    bool IsHostileHypervisorPresent()
    {
        // checking if any hypervisor is present
        int cpuInfo[CPUID_REGISTER::COUNT] = { 0 };
        __cpuid(cpuInfo, CPUID_LEAF::VERSION_AND_FEATURES);

        if ((cpuInfo[CPUID_REGISTER::ECX] & CPUID_FEATURES::HYPERVISOR_PRESENT) == 0)
        {
            return false;
        }

        __cpuid(cpuInfo, HYPERVISOR_LEAVES::INTERFACE);

        // we don't want to mess with hyper-v. someone who wants to mess
        // with the rootkit could use this to stop it from loading ;)
        if (cpuInfo[CPUID_REGISTER::EAX] == HYPERVISOR_INTERFACE_SIGNATURES::HYPER_V)
        {
            __cpuid(cpuInfo, HYPERVISOR_LEAVES::VENDOR);

            if (static_cast<UINT32>(cpuInfo[CPUID_REGISTER::EBX]) == HYPERVISOR_VENDOR_SIGNATURES::MICROSOFT_HYPER_V_EBX &&
                static_cast<UINT32>(cpuInfo[CPUID_REGISTER::ECX]) == HYPERVISOR_VENDOR_SIGNATURES::MICROSOFT_HYPER_V_ECX &&
                static_cast<UINT32>(cpuInfo[CPUID_REGISTER::EDX]) == HYPERVISOR_VENDOR_SIGNATURES::MICROSOFT_HYPER_V_EDX)
            {
                return true;
            }
        }

        return false;
    }

    bool IsVmxSupportedGlobally()
    {
        CPUID cpuInfo = { 0 };
        __cpuid(reinterpret_cast<int*>(&cpuInfo), CPUID_LEAF::VERSION_AND_FEATURES);

        if ((cpuInfo.ecx & CPUID_FEATURES::VMX) == 0)
        {
            return false;
        }

        return true;
    }

    bool IsEptSupportedGlobally()
    {
        IA32_VMX_EPT_VPID_CAP_MSR eptVpidCap = { 0 };

        eptVpidCap.All = __readmsr(static_cast<ULONG>(VMX_MSR::IA32_EPT_VPID_CAP));

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
        if (IsHostileHypervisorPresent())
        {
            LOG_ERROR("Hyper-V/VBS are active - disable them in order to run the hypervisor.");
            return false;
        }

        if (!IsVmxSupportedGlobally())
        {
            LOG_ERROR("Intel VT-x is NOT supported by the CPU.");
            return false;
        }

        if (!IsEptSupportedGlobally())
        {
            LOG_ERROR("EPT is NOT supported by the CPU.");
            return false;
        }

        if (!m_ept.Initialize())
        {
            LOG_ERROR("Failed to initialize EPT.");
            return false;
        }

        m_processorCount = KeQueryActiveProcessorCount(NULL);

        UINT64 vcpuArraySize = sizeof(Vcpu) * m_processorCount;
        m_vcpus = static_cast<Vcpu*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, vcpuArraySize, HYPERVISOR_CONFIG::VCPU_ARRAY_TAG));
        if (m_vcpus == nullptr)
        {
            LOG_ERROR("Failed to allocate virtual CPU array.");
            return false;
        }

        for (ULONG i = 0; i < m_processorCount; ++i)
        {
            // only works if we have fewer than 64 cores, which is a safe assumption for now.
            // if we had more than 64 cores, we would need to use processor groups (KeSetSystemGroupAffinityThread)
            KAFFINITY oldAffinity = KeSetSystemAffinityThreadEx(1ull << i);
            bool success = m_vcpus[i].Initialize(i, m_ept.GetEptPointer());
            KeRevertToUserAffinityThreadEx(oldAffinity);

            // if a core fails, for now we abort the entire driver startup.
            // maybe one day we work on a way to work around this for a more robust rootkit
            if (!success)
            {
                LOG_ERROR("Initialization failed on core %lu. Aborting.", i);

                // rollback process to ensure we don't destroy anything:

                for (ULONG j = 0; j < i; ++j)
                {
                    KAFFINITY rollbackAffinity = KeSetSystemAffinityThreadEx(1ull << j);
                    m_vcpus[j].Teardown();
                    KeRevertToUserAffinityThreadEx(rollbackAffinity);
                }

                ExFreePoolWithTag(m_vcpus, HYPERVISOR_CONFIG::VCPU_ARRAY_TAG);
                m_vcpus = nullptr;

                return false;
            }
        }

        LOG_INFO("SUCCESS: Hypervisor started on all cores!");
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

            ExFreePoolWithTag(m_vcpus, HYPERVISOR_CONFIG::VCPU_ARRAY_TAG);
            m_vcpus = nullptr;
        }

        m_ept.Teardown();

        LOG_INFO("Hypervisor successfully stopped and memory freed.");
    }
};
