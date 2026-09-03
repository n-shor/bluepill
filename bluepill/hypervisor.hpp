#pragma once

#include "constants.hpp"
#include "ept.hpp"
#include "raii.hpp"
#include "utils.hpp"
#include "vcpu.hpp"
#include "vmxCapabilities.hpp"
#include <intrin.h>
#include <ntifs.h>

class Hypervisor
{
private:
    Optional<PoolBuffer> m_vcpuBuffer;
    ULONG m_vcpusConstructed = 0;
    ULONG m_processorCount = 0;
    UINT64 m_systemCr3 = 0;
    VmmEpt m_ept;

    Vcpu* Vcpus() noexcept
    {
        return static_cast<Vcpu*>(m_vcpuBuffer.value().Pointer());
    }

    static bool IsHostileHypervisorPresent()
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

    static bool IsVmxSupportedGlobally()
    {
        CPUID cpuInfo = { 0 };
        __cpuid(reinterpret_cast<int*>(&cpuInfo), CPUID_LEAF::VERSION_AND_FEATURES);

        if ((cpuInfo.ecx & CPUID_FEATURES::VMX) == 0)
        {
            return false;
        }

        return true;
    }

    static UINT64 GetSystemProcessCr3()
    {
        KAPC_STATE apcState = { 0 };

        KeStackAttachProcess(reinterpret_cast<PRKPROCESS>(PsInitialSystemProcess), &apcState);
        const UINT64 systemCr3 = __readcr3();
        KeUnstackDetachProcess(&apcState);

        return systemCr3;
    }

    static bool IsEptSupportedGlobally()
    {
        Optional<IA32_VMX_EPT_VPID_CAP_MSR> eptVpidCap = VmxCapabilities::TryReadEptVpidCap();
        if (!eptVpidCap.has())
        {
            LOG_ERROR("Neither EPT nor VPID is reported in the secondary controls, "
                      "so IA32_VMX_EPT_VPID_CAP does not exist on this CPU.");
            return false;
        }

        return eptVpidCap.value().Fields.SupportPageWalkLength4 &&
               eptVpidCap.value().Fields.SupportWriteBackMemoryType &&
               eptVpidCap.value().Fields.SupportPde2mbPages;
    }

public:
    Hypervisor() = default;

    ~Hypervisor()
    {
        if (m_vcpuBuffer.has())
        {
            Vcpu* vcpus = Vcpus();
            for (ULONG i = 0; i < m_vcpusConstructed; ++i)
            {
                ScopedAffinity affinity(i);
                vcpus[i].~Vcpu();
            }
        }

        LOG_INFO("Hypervisor successfully stopped and memory freed.");
    }

    Hypervisor(const Hypervisor&) = delete;
    Hypervisor& operator=(const Hypervisor&) = delete;
    Hypervisor(Hypervisor&&) = delete;
    Hypervisor& operator=(Hypervisor&&) = delete;

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

        m_systemCr3 = GetSystemProcessCr3();
        if (m_systemCr3 == 0)
        {
            LOG_ERROR("Failed to obtain the System process CR3.");
            return false;
        }

        m_processorCount = KeQueryActiveProcessorCount(NULL);

        // only works if we have fewer than 64 cores, which is a safe assumption for now.
        // if we had more than 64 cores, we would need to use processor groups (KeSetSystemGroupAffinityThread)
        UINT64 vcpuArraySize = sizeof(Vcpu) * m_processorCount;
        m_vcpuBuffer = PoolBuffer::allocate(
            vcpuArraySize, POOL_FLAG_NON_PAGED, POOL_TAGS::VCPU_ARRAY);
        if (!m_vcpuBuffer.has())
        {
            LOG_ERROR("Failed to allocate virtual CPU array.");
            return false;
        }

        Vcpu* vcpus = Vcpus();

        for (ULONG processorIndex = 0; processorIndex < m_processorCount; ++processorIndex)
        {
            ScopedAffinity affinity(processorIndex);

            Optional<Vcpu> created = Vcpu::Create(processorIndex, m_ept.GetEptPointer(), m_systemCr3);
            if (!created.has())
            {
                LOG_ERROR("Initialization failed on core %lu. Aborting.", processorIndex);
                return false;
            }

            new (&vcpus[processorIndex]) Vcpu(static_cast<Vcpu&&>(created.value()));
            ++m_vcpusConstructed;
        }

        LOG_INFO("SUCCESS: Hypervisor started on all cores!");
        return true;
    }
};
