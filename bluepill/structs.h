#pragma once

typedef struct _CPUID
{
    int eax;
    int ebx;
    int ecx;
    int edx;
} CPUID, *PCPUID;

typedef union _IA32_FEATURE_CONTROL_MSR
{
    ULONG64 All;
    struct
    {
        ULONG64 Lock : 1;
        ULONG64 EnableSMX : 1;
        ULONG64 EnableVMXON : 1;
        ULONG64 Reserved1 : 5;
        ULONG64 EnableLocalSENTER : 7;
        ULONG64 EnableGlobalSENTER : 1;
        ULONG64 Reserved2 : 1;
        ULONG64 SGXLaunchControl : 1;
        ULONG64 SGXGlobal : 1;
        ULONG64 Reserved3 : 1;
        ULONG64 LMCE : 1;
        ULONG64 Reserved4 : 43;
    } Fields;
} IA32_FEATURE_CONTROL_MSR, *PIA32_FEATURE_CONTROL_MSR;

typedef union _IA32_VMX_BASIC_MSR
{
    ULONG64 All;
    struct
    {
        ULONG32 RevisionIdentifier : 31;
        ULONG32 Reserved1 : 1;
        ULONG32 RegionSize : 12;
        ULONG32 RegionClear : 1;
        ULONG32 Reserved2 : 3;
        ULONG32 SupportedIA64 : 1;
        ULONG32 SupportedDualMoniter : 1;
        ULONG32 MemoryType : 4;
        ULONG32 VmExitReport : 1;
        ULONG32 VmxCapabilityHint : 1;
        ULONG32 Reserved3 : 8;
    } Fields;
} IA32_VMX_BASIC_MSR, *PIA32_VMX_BASIC_MSR;
