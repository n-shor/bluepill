#include <ntddk.h>
#include <intrin.h>
#include "structs.h"

bool IsVmxSupported() {
    CPUID cpu_info = { 0 };

    static constexpr int LEAF_1 = 1;
    __cpuid(reinterpret_cast<int*>(&cpu_info), LEAF_1);

    static constexpr size_t CPUID_VMX_BIT = 1ull << 5;
    if ((cpu_info.ecx & CPUID_VMX_BIT) == 0)
    {
        return false;
    }

    IA32_FEATURE_CONTROL_MSR ia32_feature_control_msr = { 0 };
    static constexpr size_t MSR_IA32_FEATURE_CONTROL = 0x3A;
    ia32_feature_control_msr.All = __readmsr(MSR_IA32_FEATURE_CONTROL);

    if (ia32_feature_control_msr.Fields.Lock == FALSE)
    {
        ia32_feature_control_msr.Fields.Lock = TRUE;
        ia32_feature_control_msr.Fields.EnableVMXON = TRUE;
        __writemsr(MSR_IA32_FEATURE_CONTROL, ia32_feature_control_msr.All);
    }
    else if (ia32_feature_control_msr.Fields.EnableVMXON == FALSE)
    {
        return false;
    }

    return true;
}

void EnableVmx() {
    const unsigned long long old_cr4 = __readcr4();

    static constexpr size_t CR4_VMXE_BIT = 1ull << 13;
    __writecr4(old_cr4 | CR4_VMXE_BIT);
}

void DisableVmx() {
    const unsigned long long old_cr4 = __readcr4();

    static constexpr size_t CR4_VMXE_BIT = 1ull << 13;
    __writecr4(old_cr4 & (~CR4_VMXE_BIT));
}

void DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    DisableVmx();
    DbgPrint("[*] BluePill Hypervisor driver unloaded safely.\n");
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;

    DbgPrint("[+] BluePill Hypervisor driver loaded!\n");

    if (!IsVmxSupported()) {
        DbgPrint("[-] ERROR: Intel VT-x is NOT supported. Check your VM/BIOS settings.\n");
        return STATUS_NOT_SUPPORTED;
    }

    DbgPrint("[+] SUCCESS: Intel VT-x is supported!\n");

    EnableVmx();
    DbgPrint("[+] VMX enabled successfully.\n");

    return STATUS_SUCCESS;
}
