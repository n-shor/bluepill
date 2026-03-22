#include <ntddk.h>
#include <intrin.h>

void DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("[*] Hypervisor driver unloaded safely.\n");
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;

    DbgPrint("[+] BluePill Hypervisor driver loaded!\n");

    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 1);
    bool vmxSupported = (cpuInfo[2] & (1 << 5)) != 0;

    if (vmxSupported) {
        DbgPrint("[+] SUCCESS: Intel VT-x is supported! The Throne is empty.\n");
    }
    else {
        DbgPrint("[-] ERROR: Intel VT-x is NOT supported. Check your VM settings.\n");
    }

    return STATUS_SUCCESS;
}
