#include "hypervisor.hpp"
#include "utils.hpp"
#include <ntddk.h>

void __cdecl operator delete(void*, unsigned __int64)
{
    LOG_ERROR("Something is wrong - non existent delete was called.");
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

    LOG_INFO("BluePill Hypervisor driver unloaded.");
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = DriverUnload;
    LOG_INFO("BluePill Hypervisor driver loading...");

    PVOID rawMemory = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Hypervisor), HYPER_TAG);
    if (rawMemory == nullptr)
    {
        LOG_ERROR("Failed to allocate memory for g_Hypervisor.");
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
