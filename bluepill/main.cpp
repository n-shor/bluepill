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

// only one CPU tears down at a time, so a single global is fine.
extern "C" volatile bool g_ShutdownThisCpu = false;

extern "C" volatile UINT64 g_ShutdownResumeRip = 0;
extern "C" volatile UINT64 g_ShutdownGuestRsp = 0;
extern "C" volatile UINT64 g_ShutdownGuestRflags = 0;

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

typedef struct _KLDR_DATA_TABLE_ENTRY
{
    LIST_ENTRY InLoadOrderLinks;
    PVOID ExceptionTable;
    ULONG ExceptionTableSize;
    PVOID GpValue;
    PVOID NonPagedDebugInfo;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} KLDR_DATA_TABLE_ENTRY, *PKLDR_DATA_TABLE_ENTRY;

PVOID GetKernelBase(PDRIVER_OBJECT DriverObject)
{
    // DriverSection points to your driver's entry in the linked list
    PKLDR_DATA_TABLE_ENTRY entry = (PKLDR_DATA_TABLE_ENTRY)DriverObject->DriverSection;
    PKLDR_DATA_TABLE_ENTRY currentNode = entry;

    if (!entry)
    {
        return nullptr;
    }

    // Walk the doubly-linked list
    do
    {
        if (currentNode->BaseDllName.Buffer != nullptr && currentNode->BaseDllName.Length > 0)
        {
            // Check if the current module is the kernel
            if (_wcsnicmp(currentNode->BaseDllName.Buffer, L"ntoskrnl.exe", 12) == 0 ||
                _wcsnicmp(currentNode->BaseDllName.Buffer, L"ntkrnlmp.exe", 12) == 0)
            {
                return currentNode->DllBase;
            }
        }

        // Move to the next module in the list
        currentNode = (PKLDR_DATA_TABLE_ENTRY)currentNode->InLoadOrderLinks.Flink;

    } while (currentNode != entry); // Stop if we loop all the way back to our own driver

    return nullptr;
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
