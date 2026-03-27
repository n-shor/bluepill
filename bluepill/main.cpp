#include <ntddk.h>
#include <intrin.h>

void DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("[*] Hypervisor driver unloaded safely.\n");
}

bool isVmxSupported()
{
    constexpr static size_t CPU_ID_OUTPUT_SIZE = 4;
    int cpuInfo[CPU_ID_OUTPUT_SIZE] = { 0 };

    constexpr static int LEAF_1 = 1;
    __cpuid(cpuInfo, LEAF_1);

    constexpr static size_t ECX = 2;
    constexpr static size_t VMX_BIT = 1 << 5;
    return (cpuInfo[ECX] & VMX_BIT) != 0;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;

    DbgPrint("[+] BluePill Hypervisor driver loaded!\n");

    if (isVmxSupported()) {
        DbgPrint("[+] SUCCESS: Intel VT-x is supported! The Throne is empty.\n");
    }
    else {
        DbgPrint("[-] ERROR: Intel VT-x is NOT supported. Check your VM settings.\n");
    }

    return STATUS_SUCCESS;
}
