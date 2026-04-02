#include <ntddk.h>
#include "hypervisor.hpp"

inline void* __cdecl operator new(size_t, void* p)
{
    return p;
}

void __cdecl operator delete(void*, unsigned __int64)
{
    DbgPrint("[!] PROBLEM: Something is wrong - non existent delete was called.\n");
}

Hypervisor* g_Hypervisor = nullptr;
// change later to 'erhT' to make it less obvious
static constexpr ULONG HYPER_TAG = 'pyhG';

void DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_Hypervisor != nullptr)
    {
        g_Hypervisor->~Hypervisor();

        ExFreePoolWithTag(g_Hypervisor, HYPER_TAG);
        g_Hypervisor = nullptr;
    }

    DbgPrint("[*] BluePill Hypervisor driver unloaded.\n");
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;
    DbgPrint("[+] BluePill Hypervisor driver loading...\n");

    PVOID rawMemory = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Hypervisor), HYPER_TAG);
    if (rawMemory == nullptr)
    {
        DbgPrint("[-] ERROR: Failed to allocate memory for g_Hypervisor.\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    g_Hypervisor = new (rawMemory) Hypervisor();

    if (!g_Hypervisor->Start())
    {
        g_Hypervisor->~Hypervisor();
        ExFreePoolWithTag(g_Hypervisor, HYPER_TAG);
        g_Hypervisor = nullptr;

        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}
