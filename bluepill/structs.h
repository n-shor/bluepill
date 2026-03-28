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
        ULONG64 Reserved : 5;
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
