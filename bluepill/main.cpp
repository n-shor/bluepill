#include <ntddk.h>
#include "hypervisor.hpp"

Hypervisor g_Hypervisor;

void DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    g_Hypervisor.Stop();
    DbgPrint("[*] BluePill Hypervisor driver unloaded.\n");
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;
    DbgPrint("[+] BluePill Hypervisor driver loading...\n");

    if (!g_Hypervisor.Start()) {
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}
