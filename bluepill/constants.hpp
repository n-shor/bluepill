#pragma once
#include <ntifs.h>

enum class VMCS_FIELDS : UINT64
{
    VIRTUAL_PROCESSOR_ID = 0x00000000,

    GUEST_ES_SELECTOR = 0x00000800,
    GUEST_CS_SELECTOR = 0x00000802,
    GUEST_SS_SELECTOR = 0x00000804,
    GUEST_DS_SELECTOR = 0x00000806,
    GUEST_FS_SELECTOR = 0x00000808,
    GUEST_GS_SELECTOR = 0x0000080A,
    GUEST_LDTR_SELECTOR = 0x0000080C,
    GUEST_TR_SELECTOR = 0x0000080E,

    HOST_ES_SELECTOR = 0x00000C00,
    HOST_CS_SELECTOR = 0x00000C02,
    HOST_SS_SELECTOR = 0x00000C04,
    HOST_DS_SELECTOR = 0x00000C06,
    HOST_FS_SELECTOR = 0x00000C08,
    HOST_GS_SELECTOR = 0x00000C0A,
    HOST_TR_SELECTOR = 0x00000C0C,

    MSR_BITMAP = 0x00002004,
    VIRTUAL_APIC_ADDRESS = 0x00002012,
    EPT_POINTER = 0x0000201A,
    GUEST_PHYSICAL_ADDRESS = 0x00002400,
    VMCS_LINK_POINTER = 0x00002800,

    PIN_BASED_VM_EXEC_CONTROL = 0x00004000,
    PRIMARY_CPU_BASED_VM_EXEC_CONTROL = 0x00004002,
    EXCEPTION_BITMAP = 0x00004004,
    CR3_TARGET_COUNT = 0x0000400A,
    VM_EXIT_CONTROLS = 0x0000400C,
    VM_ENTRY_CONTROLS = 0x00004012,
    VM_ENTRY_INTERRUPTION_INFO = 0x00004016,
    VM_ENTRY_EXCEPTION_ERROR_CODE = 0x00004018,
    VM_ENTRY_INSTRUCTION_LENGTH = 0x0000401A,
    SECONDARY_CPU_BASED_VM_EXEC_CONTROL = 0x0000401E,

    VM_INSTRUCTION_ERROR = 0x00004400,
    VM_EXIT_REASON = 0x00004402,
    VM_EXIT_INSTRUCTION_LEN = 0x0000440C,

    GUEST_ES_LIMIT = 0x00004800,
    GUEST_CS_LIMIT = 0x00004802,
    GUEST_SS_LIMIT = 0x00004804,
    GUEST_DS_LIMIT = 0x00004806,
    GUEST_FS_LIMIT = 0x00004808,
    GUEST_GS_LIMIT = 0x0000480A,
    GUEST_LDTR_LIMIT = 0x0000480C,
    GUEST_TR_LIMIT = 0x0000480E,
    GUEST_GDTR_LIMIT = 0x00004810,
    GUEST_IDTR_LIMIT = 0x00004812,
    GUEST_ES_ACCESS_RIGHTS = 0x00004814,
    GUEST_CS_ACCESS_RIGHTS = 0x00004816,
    GUEST_SS_ACCESS_RIGHTS = 0x00004818,
    GUEST_DS_ACCESS_RIGHTS = 0x0000481A,
    GUEST_FS_ACCESS_RIGHTS = 0x0000481C,
    GUEST_GS_ACCESS_RIGHTS = 0x0000481E,
    GUEST_LDTR_ACCESS_RIGHTS = 0x00004820,
    GUEST_TR_ACCESS_RIGHTS = 0x00004822,
    GUEST_INTERRUPTIBILITY_STATE = 0x00004824,
    GUEST_ACTIVITY_STATE = 0x00004826,
    GUEST_SYSENTER_CS = 0x0000482A,

    HOST_IA32_SYSENTER_CS = 0x00004C00,

    CR0_GUEST_HOST_MASK = 0x00006000,
    CR4_GUEST_HOST_MASK = 0x00006002,
    CR0_READ_SHADOW = 0x00006004,
    CR4_READ_SHADOW = 0x00006006,

    VM_EXIT_QUALIFICATION = 0x00006400,

    GUEST_CR0 = 0x00006800,
    GUEST_CR3 = 0x00006802,
    GUEST_CR4 = 0x00006804,
    GUEST_ES_BASE = 0x00006806,
    GUEST_CS_BASE = 0x00006808,
    GUEST_SS_BASE = 0x0000680A,
    GUEST_DS_BASE = 0x0000680C,
    GUEST_FS_BASE = 0x0000680E,
    GUEST_GS_BASE = 0x00006810,
    GUEST_LDTR_BASE = 0x00006812,
    GUEST_TR_BASE = 0x00006814,
    GUEST_GDTR_BASE = 0x00006816,
    GUEST_IDTR_BASE = 0x00006818,
    GUEST_RSP = 0x0000681C,
    GUEST_RIP = 0x0000681E,
    GUEST_DR7 = 0x0000681A,
    GUEST_RFLAGS = 0x00006820,
    GUEST_PENDING_DEBUG_EXCEPTIONS = 0x00006822,
    GUEST_SYSENTER_ESP = 0x00006824,
    GUEST_SYSENTER_EIP = 0x00006826,

    HOST_CR0 = 0x00006C00,
    HOST_CR3 = 0x00006C02,
    HOST_CR4 = 0x00006C04,
    HOST_FS_BASE = 0x00006C06,
    HOST_GS_BASE = 0x00006C08,
    HOST_TR_BASE = 0x00006C0A,
    HOST_GDTR_BASE = 0x00006C0C,
    HOST_IDTR_BASE = 0x00006C0E,
    HOST_IA32_SYSENTER_ESP = 0x00006C10,
    HOST_IA32_SYSENTER_EIP = 0x00006C12,
    HOST_RSP = 0x00006C14,
    HOST_RIP = 0x00006C16,
};

enum class IA32_SYSTEM_MSR : UINT64
{
    FEATURE_CONTROL = 0x0000003A,
    FS_BASE = 0xC0000100,
    GS_BASE = 0xC0000101,
    SYSENTER_CS = 0x00000174,
    SYSENTER_ESP = 0x00000175,
    SYSENTER_EIP = 0x00000176,
};

enum class IA32_VMX_MSR : UINT64
{
    BASIC = 0x00000480,
    PINBASED_CTLS = 0x00000481,
    PROCBASED_CTLS = 0x00000482,
    EXIT_CTLS = 0x00000483,
    ENTRY_CTLS = 0x00000484,
    PROCBASED_CTLS2 = 0x0000048B,
    EPT_VPID_CAP = 0x0000048C,
    TRUE_PINBASED_CTLS = 0x0000048D,
    TRUE_PROCBASED_CTLS = 0x0000048E,
    TRUE_EXIT_CTLS = 0x0000048F,
    TRUE_ENTRY_CTLS = 0x00000490,
};

namespace POOL_TAGS
{
#if DBG
inline constexpr ULONG HYPERVISOR = 'pyhG';
inline constexpr ULONG VCPU_ARRAY = 'upcV';
inline constexpr ULONG STACK = 'kStS';
inline constexpr ULONG HOST_GDT = 'tDGh';
inline constexpr ULONG EPT_TABLE = 'TPEV';
#else
inline constexpr ULONG SHARED = 'lbtC';
inline constexpr ULONG HYPERVISOR = SHARED;
inline constexpr ULONG VCPU_ARRAY = SHARED;
inline constexpr ULONG STACK = SHARED;
inline constexpr ULONG HOST_GDT = SHARED;
inline constexpr ULONG EPT_TABLE = SHARED;
#endif
} // namespace POOL_TAGS

namespace HYPERVISOR_CONFIG
{
inline constexpr UINT64 STACK_SIZE = 0x8000;
inline constexpr UINT64 SHUTDOWN_HYPERCALL = 0xDEADDEADDEADull;
} // namespace HYPERVISOR_CONFIG

namespace HYPERVISOR_LEAVES
{
inline constexpr UINT64 VENDOR = 0x40000000;
inline constexpr UINT64 INTERFACE = 0x40000001;
} // namespace HYPERVISOR_LEAVES

// combines 4 given characters into a UINT32 representation of a string.
// this is needed because writing the string in reversed order (like 'dcba')
// is reliant on compiler implementation.
inline consteval UINT32 PackSignature(char a, char b, char c, char d)
{
    constexpr UINT32 BITS_IN_CHAR = 8;

    return (static_cast<UINT32>(a)) |
           (static_cast<UINT32>(b) << BITS_IN_CHAR) |
           (static_cast<UINT32>(c) << (BITS_IN_CHAR + BITS_IN_CHAR)) |
           (static_cast<UINT32>(d) << (BITS_IN_CHAR + BITS_IN_CHAR + BITS_IN_CHAR));
}

namespace HYPERVISOR_INTERFACE_SIGNATURES
{
inline constexpr UINT32 HYPER_V = PackSignature('H', 'v', '#', '1');
} // namespace HYPERVISOR_INTERFACE_SIGNATURES

namespace HYPERVISOR_VENDOR_SIGNATURES
{
// this turns into "Microsoft Hv"
inline constexpr UINT32 MICROSOFT_HYPER_V_EBX = PackSignature('M', 'i', 'c', 'r');
inline constexpr UINT32 MICROSOFT_HYPER_V_ECX = PackSignature('o', 's', 'o', 'f');
inline constexpr UINT32 MICROSOFT_HYPER_V_EDX = PackSignature('t', ' ', 'H', 'v');
} // namespace HYPERVISOR_VENDOR_SIGNATURES

namespace CR4_FLAGS
{
inline constexpr UINT64 VMXE = 1ull << 13;
} // namespace CR4_FLAGS

namespace CPUID_FEATURES
{
inline constexpr UINT64 VMX = 1ull << 5;
inline constexpr UINT64 OSXSAVE = 1ull << 27;
inline constexpr UINT64 AVX = 1ull << 28;
inline constexpr UINT64 HYPERVISOR_PRESENT = 1ull << 31;
} // namespace CPUID_FEATURES

namespace XCR0
{
inline constexpr unsigned int INDEX = 0; // XCR0 = extended control register 0

inline constexpr UINT64 X87 = 1ull << 0;
inline constexpr UINT64 SSE = 1ull << 1;
inline constexpr UINT64 YMM = 1ull << 2;

// VEX encoded 256 bit instructions need both state components enabled
inline constexpr UINT64 AVX_STATE = SSE | YMM;
} // namespace XCR0

namespace CPUID_REGISTER
{
inline constexpr UINT64 COUNT = 4;
inline constexpr UINT64 EAX = 0;
inline constexpr UINT64 EBX = 1;
inline constexpr UINT64 ECX = 2;
inline constexpr UINT64 EDX = 3;
} // namespace CPUID_REGISTER

namespace CPUID_LEAF
{
inline constexpr UINT64 VERSION_AND_FEATURES = 1;
}

namespace PRIMARY_CONTROLS
{
inline constexpr UINT32 RDTSC_EXITING = 1ul << 12;
inline constexpr UINT32 USE_TPR_SHADOW = 1ul << 21;
inline constexpr UINT32 USE_MSR_BITMAPS = 1ul << 28;
inline constexpr UINT32 ACTIVATE_SECONDARY_CONTROLS = 1ul << 31;
} // namespace PRIMARY_CONTROLS

namespace SECONDARY_CONTROLS
{
inline constexpr UINT32 ENABLE_EPT = 1ul << 1;
inline constexpr UINT32 ENABLE_RDTSCP = 1ul << 3;
inline constexpr UINT32 ENABLE_VPID = 1ul << 5;
inline constexpr UINT32 ENABLE_INVPCID = 1ul << 12;
inline constexpr UINT32 ENABLE_XSAVES = 1ul << 20;
} // namespace SECONDARY_CONTROLS

namespace EXIT_CONTROLS
{
inline constexpr UINT32 HOST_ADDRESS_SPACE_SIZE = 1ul << 9;
} // namespace EXIT_CONTROLS

namespace ENTRY_CONTROLS
{
inline constexpr UINT32 IA32E_MODE_GUEST = 1ul << 9;
} // namespace ENTRY_CONTROLS

namespace PHYSICAL_MEMORY
{
inline constexpr UINT64 INVALID_POINTER = ~0ull;
} // namespace PHYSICAL_MEMORY

namespace MEMORY_TYPES
{
inline constexpr UINT64 UNCACHEABLE = 0;
inline constexpr UINT64 WRITEBACK = 6;
} // namespace MEMORY_TYPES

namespace EPT_CONFIG
{
inline constexpr UINT64 PAGE_WALK_LENGTH_4 = 3;
inline constexpr UINT64 MAX_ENTRY_COUNT = 512;
inline constexpr UINT64 SIZE_2MB = 2ull * 1024 * 1024;
inline constexpr UINT64 VGA_MEMORY_START_PFN = 0xA0;
inline constexpr UINT64 BIOS_MEMORY_END_PFN = 0xFF;
} // namespace EPT_CONFIG

namespace EPT_SHIFTS
{
inline constexpr UINT64 BITS_PER_LEVEL = 9;
inline constexpr UINT64 PT = 12;
inline constexpr UINT64 PD = PT + BITS_PER_LEVEL;
inline constexpr UINT64 PDPT = PD + BITS_PER_LEVEL;
inline constexpr UINT64 PML4 = PDPT + BITS_PER_LEVEL;
inline constexpr UINT64 INDEX_MASK = 0x1FF; // 9 bits for 512 indices
} // namespace EPT_SHIFTS

namespace SEGMENT_ACCESS_RIGHTS
{
inline constexpr ULONG UNUSABLE = (1 << 16);
} // namespace SEGMENT_ACCESS_RIGHTS

namespace SEGMENT_SHIFTS
{
inline constexpr ULONG BASE_MIDDLE = 16;
inline constexpr ULONG BASE_HIGH = 24;
inline constexpr ULONG BASE_UPPER = 32;

inline constexpr ULONG LIMIT_HIGH = 16;

inline constexpr ULONG AR_TYPE = 0;
inline constexpr ULONG AR_SYSTEM = 4;
inline constexpr ULONG AR_DPL = 5;
inline constexpr ULONG AR_DPL_MASK = 0b11; // DPL is 2 bits wide
inline constexpr ULONG AR_PRESENT = 7;
inline constexpr ULONG AR_AVL = 12;
inline constexpr ULONG AR_LONG_MODE = 13;
inline constexpr ULONG AR_DEFAULT_BIG = 14;
inline constexpr ULONG AR_GRANULARITY = 15;
inline constexpr ULONG AR_UNUSABLE = 16;
} // namespace SEGMENT_SHIFTS

namespace BITS_16
{
inline constexpr UINT64 HIGH_SHIFT = 16; // shifting to / from the high half of a 32-bit value
} // namespace BITS_16

namespace BITS_32
{
inline constexpr UINT64 LOW_MASK = 0xFFFFFFFFull; // masking out the high 32 bits
inline constexpr UINT64 HIGH_SHIFT = 32;          // shifting to / from the high half of a 64-bit value
} // namespace BITS_32

namespace EXCEPTION_VECTORS
{
inline constexpr UINT32 UD = 6;  // undefined opcode
inline constexpr UINT32 GP = 13; // general protection
} // namespace EXCEPTION_VECTORS

namespace EXCEPTION_ERROR_CODES
{
inline constexpr UINT32 GP_NON_SEGMENT = 0;
} // namespace EXCEPTION_ERROR_CODES

namespace CPL
{
inline constexpr UINT64 KERNEL = 0;
inline constexpr UINT64 USER = 3;
} // namespace CPL

namespace VM_ENTRY_INTERRUPTION
{
inline constexpr UINT32 VALID = 1u << 31;
inline constexpr UINT32 TYPE_HARDWARE_EXCEPTION = 3u << 8;
inline constexpr UINT32 DELIVER_ERROR_CODE = 1u << 11;
} // namespace VM_ENTRY_INTERRUPTION

enum class VMEXIT_REASON : UINT64
{
    NMI_EXCEPTION = 0,
    EXTERNAL_INTERRUPT = 1,
    TRIPLE_FAULT = 2,
    CPUID_EXIT = 10,
    HLT = 12,
    INVD = 13,
    INVLPG = 14,
    RDTSC = 16,
    VMCALL = 18,
    VMCLEAR = 19,
    VMLAUNCH = 20,
    VMPTRLD = 21,
    VMPTRST = 22,
    VMREAD = 23,
    VMRESUME = 24,
    VMWRITE = 25,
    VMXOFF = 26,
    VMXON = 27,
    CR_ACCESS = 28,
    DR_ACCESS = 29,
    IO_INSTRUCTION = 30,
    RDMSR = 31,
    WRMSR = 32,
    INVALID_GUEST_STATE = 33,
    MWAIT = 36,
    MONITOR = 39,
    PAUSE = 40,
    EPT_VIOLATION = 48,
    EPT_MISCONFIG = 49,
    INVEPT = 50,
    RDTSCP = 51,
    INVVPID = 53,
    XSETBV = 55,
};

namespace VMEXIT_REASON_MASKS
{
inline constexpr UINT32 BASIC_REASON = 0xFFFF;
inline constexpr UINT32 ENTRY_FAILURE_FLAG = 1ul << 31;
} // namespace VMEXIT_REASON_MASKS

namespace BUGCHECK_CODES
{
inline constexpr ULONG VM_ENTRY_FAILURE = 0xDEAD0001;
inline constexpr ULONG EPT_VIOLATION = 0xDEAD0002;
inline constexpr ULONG UNHANDLED_EXIT = 0xDEAD0003;
inline constexpr ULONG VMRESUME_FAILURE = 0xDEAD0004;
inline constexpr ULONG VMXOFF_FAILURE = 0xDEAD0005;
} // namespace BUGCHECK_CODES

namespace GDT_CONSTANTS
{
inline constexpr UINT64 SYSTEM_SEGMENT_FLAG = 0;
inline constexpr UINT32 NULL_SELECTOR_INDEX = 0;
inline constexpr UINT64 GDT_ENTRY_SIZE = 8;
inline constexpr UINT64 SYSTEM_DESCRIPTOR_SIZE = 16;
inline constexpr UINT64 ALIGNMENT = 8;
inline constexpr UINT64 ALIGNMENT_MASK = ALIGNMENT - 1;
inline constexpr UINT32 VMX_HOST_TR_LIMIT = 0x67;
} // namespace GDT_CONSTANTS

namespace VMX_RESULT
{
inline constexpr unsigned char SUCCESS = 0;
inline constexpr unsigned char FAIL_INVALID = 1; // no current VMCS
inline constexpr unsigned char FAIL_VALID = 2;   // VMCS loaded but instruction rejected
} // namespace VMX_RESULT

namespace DESCRIPTOR_TYPES
{
namespace TSS
{
inline constexpr UINT64 AVAILABLE = 9;
inline constexpr UINT64 BUSY = 11;
} // namespace TSS
} // namespace DESCRIPTOR_TYPES
