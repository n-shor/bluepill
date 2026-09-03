#pragma once

#include "constants.hpp"
#include "structs.hpp"
#include "utils.hpp"
#include <intrin.h>
#include <ntifs.h>

namespace VmxCapabilities
{
inline bool UsesTrueControlMsrs() noexcept
{
    IA32_VMX_BASIC_MSR vmxBasic = { 0 };
    // safe to read as soon as CPUID reports VMX support
    vmxBasic.All = __readmsr(static_cast<ULONG>(IA32_VMX_MSR::BASIC));

    return vmxBasic.Fields.SupportsTrueControlMsrs != 0;
}

inline UINT64 ReadControlMsr(bool useTrueMsrs, IA32_VMX_MSR trueMsr, IA32_VMX_MSR legacyMsr) noexcept
{
    return __readmsr(static_cast<ULONG>(useTrueMsrs ? trueMsr : legacyMsr));
}

inline bool IsControlAllowed(UINT64 capabilityMsrValue, UINT32 controlBit) noexcept
{
    LARGE_INTEGER msr = { 0 };
    msr.QuadPart = capabilityMsrValue;

    const ULONG allowedOne = static_cast<ULONG>(msr.HighPart);
    return (allowedOne & controlBit) != 0;
}

// IA32_VMX_EPT_VPID_CAP only exists when EPT or VPID is reported available in the
// secondary controls, and the secondary controls themselves only exist when the
// primary controls allow ACTIVATE_SECONDARY_CONTROLS. returns empty when the MSR is not architecturally present.
inline Optional<IA32_VMX_EPT_VPID_CAP_MSR> TryReadEptVpidCap() noexcept
{
    const bool useTrueMsrs = UsesTrueControlMsrs();

    const UINT64 primaryControls = ReadControlMsr(
        useTrueMsrs, IA32_VMX_MSR::TRUE_PROCBASED_CTLS, IA32_VMX_MSR::PROCBASED_CTLS);

    if (!IsControlAllowed(primaryControls, PRIMARY_CONTROLS::ACTIVATE_SECONDARY_CONTROLS))
    {
        return Optional<IA32_VMX_EPT_VPID_CAP_MSR>();
    }

    const UINT64 secondaryControls = __readmsr(static_cast<ULONG>(IA32_VMX_MSR::PROCBASED_CTLS2));

    if (!IsControlAllowed(secondaryControls, SECONDARY_CONTROLS::ENABLE_EPT) &&
        !IsControlAllowed(secondaryControls, SECONDARY_CONTROLS::ENABLE_VPID))
    {
        return Optional<IA32_VMX_EPT_VPID_CAP_MSR>();
    }

    IA32_VMX_EPT_VPID_CAP_MSR eptVpidCap = { 0 };
    eptVpidCap.All = __readmsr(static_cast<ULONG>(IA32_VMX_MSR::EPT_VPID_CAP));

    return Optional<IA32_VMX_EPT_VPID_CAP_MSR>(eptVpidCap);
}
} // namespace VmxCapabilities
