#pragma once

#include <ntifs.h>

typedef struct _CPUID
{
    int eax;
    int ebx;
    int ecx;
    int edx;
} CPUID, *PCPUID;

typedef union _IA32_FEATURE_CONTROL_MSR
{
    UINT64 All;
    struct
    {
        UINT64 Lock : 1;
        UINT64 EnableSMX : 1;
        UINT64 EnableVMXON : 1;
        UINT64 Reserved1 : 5;
        UINT64 EnableLocalSENTER : 7;
        UINT64 EnableGlobalSENTER : 1;
        UINT64 Reserved2 : 1;
        UINT64 SGXLaunchControl : 1;
        UINT64 SGXGlobal : 1;
        UINT64 Reserved3 : 1;
        UINT64 LMCE : 1;
        UINT64 Reserved4 : 43;
    } Fields;
} IA32_FEATURE_CONTROL_MSR, *PIA32_FEATURE_CONTROL_MSR;

typedef union _IA32_VMX_BASIC_MSR
{
    UINT64 All;
    struct
    {
        UINT64 RevisionIdentifier : 31;
        UINT64 Reserved1 : 1;
        UINT64 RegionSize : 13;
        UINT64 Reserved2 : 3;
        UINT64 AddressesLimitedTo32Bits : 1;
        UINT64 SupportsDualMonitorSmm : 1;
        UINT64 MemoryType : 4;
        UINT64 ReportsInsOutsExitInformation : 1;
        UINT64 SupportsTrueControlMsrs : 1;
        UINT64 SupportsAnyVectorExceptionInjection : 1;
        UINT64 Reserved3 : 7;
    } Fields;
} IA32_VMX_BASIC_MSR, *PIA32_VMX_BASIC_MSR;

typedef union _IA32_VMX_EPT_VPID_CAP_MSR
{
    UINT64 All;
    struct
    {
        UINT64 SupportExecuteOnlyPages : 1;
        UINT64 Reserved1 : 5;
        UINT64 SupportPageWalkLength4 : 1;
        UINT64 Reserved2 : 1;
        UINT64 SupportUncacheableMemoryType : 1;
        UINT64 Reserved3 : 5;
        UINT64 SupportWriteBackMemoryType : 1;
        UINT64 Reserved4 : 1;
        UINT64 SupportPde2mbPages : 1;
        UINT64 SupportPdpte1GbPages : 1;
        UINT64 Reserved5 : 2;
        UINT64 SupportInvept : 1;
        UINT64 SupportAccessedAndDirtyFlag : 1;
        UINT64 Reserved6 : 3;
        UINT64 SupportSingleContextInvept : 1;
        UINT64 SupportAllContextInvept : 1;
        UINT64 Reserved7 : 5;
        UINT64 SupportInvvpid : 1;
        UINT64 Reserved8 : 7;
        UINT64 SupportIndividualAddressInvvpid : 1;
        UINT64 SupportSingleContextInvvpid : 1;
        UINT64 SupportAllContextInvvpid : 1;
        UINT64 SupportSingleContextRetainingGlobalsInvvpid : 1;
        UINT64 Reserved9 : 20;
    } Fields;
} IA32_VMX_EPT_VPID_CAP_MSR, *PIA32_VMX_EPT_VPID_CAP_MSR;

typedef union _EPT_POINTER
{
    UINT64 All;
    struct
    {
        UINT64 MemoryType : 3;
        UINT64 PageWalkLength : 3;
        UINT64 EnableAccessedAndDirtyFlags : 1;
        UINT64 Reserved1 : 5;
        UINT64 PageMapLevel4Address : 36;
        UINT64 Reserved2 : 16;
    } Fields;
} EPT_POINTER, *PEPT_POINTER;

typedef union _EPT_PML4E
{
    UINT64 All;
    struct
    {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 Reserved1 : 5;
        UINT64 Accessed : 1;
        UINT64 Ignored1 : 1;
        UINT64 ExecuteAccessForUserModeLinearAddress : 1;
        UINT64 Ignored2 : 1;
        UINT64 PageDirectoryPointerTableAddress : 36;
        UINT64 Reserved2 : 4;
        UINT64 Ignored3 : 12;
    } Fields;
} EPT_PML4E, *PEPT_PML4E;

typedef union _EPT_PDPTE
{
    UINT64 All;
    struct
    {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 Reserved1 : 5;
        UINT64 Accessed : 1;
        UINT64 Ignored1 : 1;
        UINT64 ExecuteAccessForUserModeLinearAddress : 1;
        UINT64 Ignored2 : 1;
        UINT64 PageDirectoryAddress : 36;
        UINT64 Reserved2 : 4;
        UINT64 Ignored3 : 12;
    } Fields;
} EPT_PDPTE, *PEPT_PDPTE;

typedef union _EPT_PDE
{
    UINT64 All;
    struct
    {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 Reserved1 : 5;
        UINT64 Accessed : 1;
        UINT64 Ignored1 : 1;
        UINT64 ExecuteAccessForUserModeLinearAddress : 1;
        UINT64 Ignored2 : 1;
        UINT64 PageTableAddress : 36;
        UINT64 Reserved2 : 4;
        UINT64 Ignored3 : 12;
    } Fields;
} EPT_PDE, *PEPT_PDE;

typedef union _EPT_PTE
{
    UINT64 All;
    struct
    {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 EPTMemoryType : 3;
        UINT64 IgnorePAT : 1;
        UINT64 Ignored1 : 1;
        UINT64 Accessed : 1;
        UINT64 Dirty : 1;
        UINT64 ExecuteAccessForUserModeLinearAddress : 1;
        UINT64 Ignored2 : 1;
        UINT64 PageAddress : 36;
        UINT64 Reserved : 4;
        UINT64 Ignored3 : 11;
        UINT64 SuppressVE : 1;
    } Fields;
} EPT_PTE, *PEPT_PTE;

// we want there to be no alignment padding to ensure all the fields
// get populated correctly when using intrinsics
#pragma pack(push, 1)

struct SYSTEM_DESCRIPTOR_TABLE_REGISTER
{
    UINT16 Limit;
    UINT64 Base;
};

union SEGMENT_SELECTOR
{
    UINT16 All;
    struct
    {
        UINT64 RPL : 2;    // requested privilege level
        UINT64 TI : 1;     // table indicator
        UINT64 Index : 13; // index into the array
    } Fields;
};

union SEGMENT_DESCRIPTOR
{
    UINT64 All;
    struct
    {
        UINT64 LimitLow : 16;
        UINT64 BaseLow : 16;
        UINT64 BaseMiddle : 8;
        UINT64 Type : 4;
        UINT64 System : 1;
        UINT64 DPL : 2;
        UINT64 Present : 1;
        UINT64 LimitHigh : 4;
        UINT64 AVL : 1;
        UINT64 LongMode : 1;
        UINT64 DefaultBig : 1;
        UINT64 Granularity : 1;
        UINT64 BaseHigh : 8;
    } Fields;
};

struct SYSTEM_SEGMENT_DESCRIPTOR_64
{
    SEGMENT_DESCRIPTOR BaseDescriptor;
    // the 64 bit extension
    UINT32 BaseUpper32;
    UINT32 Reserved;
};

#pragma pack(pop)

struct SEGMENT_INFO
{
    UINT64 Base;
    UINT32 Limit;
    UINT32 AccessRights;
};

struct GUEST_REGISTERS
{
    UINT64 Rax;
    UINT64 Rcx;
    UINT64 Rdx;
    UINT64 Rbx;
    UINT64 Rbp;
    UINT64 Rsi;
    UINT64 Rdi;
    UINT64 R8;
    UINT64 R9;
    UINT64 R10;
    UINT64 R11;
    UINT64 R12;
    UINT64 R13;
    UINT64 R14;
    UINT64 R15;
};

typedef union _EPT_PDE_2MB
{
    UINT64 All;
    struct
    {
        UINT64 ReadAccess : 1;
        UINT64 WriteAccess : 1;
        UINT64 ExecuteAccess : 1;
        UINT64 EPTMemoryType : 3;
        UINT64 IgnorePAT : 1;
        UINT64 LargePage : 1; // must be 1
        UINT64 Accessed : 1;
        UINT64 Dirty : 1;
        UINT64 ExecuteAccessForUserModeLinearAddress : 1;
        UINT64 Ignored1 : 1;
        UINT64 Reserved1 : 9;
        UINT64 PageAddress : 31;
        UINT64 Ignored2 : 11;
        UINT64 SuppressVE : 1;
    } Fields;
} EPT_PDE_2MB, *PEPT_PDE_2MB;

struct INVEPT_DESCRIPTOR
{
    UINT64 EptPointer;
    UINT64 Reserved;
};

namespace HOST_STACK
{
inline constexpr UINT64 ALIGNMENT = 16;
}; // namespace HOST_STACK

struct alignas(HOST_STACK::ALIGNMENT) HOST_STACK_CONTEXT
{
};
