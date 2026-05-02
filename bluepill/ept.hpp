#pragma once

#include "structs.h"
#include "utils.hpp"
#include <ntddk.h>

static constexpr size_t ADDRESS_SIZE_BITS = 48;
static constexpr size_t OFFSET_SIZE_BITS = 9;

static constexpr size_t PFN_SIZE_BITS = ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 4);

static constexpr size_t MEMORY_TYPE_WRITEBACK = 6;
static constexpr size_t EPT_PAGE_WALK_LENGTH_4 = 3;

static constexpr size_t MAX_ENTRY_COUNT = PAGE_SIZE / sizeof(UINT64);

// remember to change the pool tag to something less obvious later on
static constexpr ULONG EPT_POOL_TAG = 'TPEV';

class VmmEpt
{
public:
    VmmEpt()
    {
        this->Pml4VirtualAddress = nullptr;
        this->Pml4PhysicalAddress = 0;
        this->EptPointer.All = 0;
    }

    ~VmmEpt()
    {
        Teardown();
    }

#if DBG
    bool VerifyEptMapping()
    {
        volatile ULONG testVariable = 0x1337;

        PHYSICAL_ADDRESS truePhysicalAddress = MmGetPhysicalAddress((PVOID)&testVariable);
        UINT64 expectedPfn = truePhysicalAddress.QuadPart >> PFN_SIZE_BITS;

        UINT64 pml4Offset = (truePhysicalAddress.QuadPart >> (ADDRESS_SIZE_BITS - OFFSET_SIZE_BITS)) & ((1ull << OFFSET_SIZE_BITS) - 1);
        UINT64 pdptOffset = (truePhysicalAddress.QuadPart >> (ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 2))) & ((1ull << OFFSET_SIZE_BITS) - 1);
        UINT64 pdOffset = (truePhysicalAddress.QuadPart >> (ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 3))) & ((1ull << OFFSET_SIZE_BITS) - 1);
        UINT64 ptOffset = (truePhysicalAddress.QuadPart >> (ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 4))) & ((1ull << OFFSET_SIZE_BITS) - 1);

        PEPT_PML4E pml4Entry = &this->Pml4VirtualAddress[pml4Offset];
        if (pml4Entry->Fields.ReadAccess == 0)
        {
            return false;
        }

        PHYSICAL_ADDRESS pdptPhys;
        pdptPhys.QuadPart = (static_cast<ULONG64>(pml4Entry->Fields.PageDirectoryPointerTableAddress)) << PFN_SIZE_BITS;
        PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(MmGetVirtualForPhysical(pdptPhys));
        if (!pdptTable || pdptTable[pdptOffset].Fields.ReadAccess == 0)
        {
            return false;
        }

        PHYSICAL_ADDRESS pdPhys;
        pdPhys.QuadPart = (static_cast<ULONG64>(pdptTable[pdptOffset].Fields.PageDirectoryAddress)) << PFN_SIZE_BITS;
        PEPT_PDE pdTable = static_cast<PEPT_PDE>(MmGetVirtualForPhysical(pdPhys));
        if (!pdTable || pdTable[pdOffset].Fields.ReadAccess == 0)
        {
            return false;
        }

        PHYSICAL_ADDRESS ptPhys;
        ptPhys.QuadPart = (static_cast<ULONG64>(pdTable[pdOffset].Fields.PageTableAddress)) << PFN_SIZE_BITS;
        PEPT_PTE ptTable = static_cast<PEPT_PTE>(MmGetVirtualForPhysical(ptPhys));
        if (!ptTable || ptTable[ptOffset].Fields.ReadAccess == 0)
        {
            return false;
        }

        UINT64 mappedPfn = ptTable[ptOffset].Fields.PageAddress;

        if (mappedPfn == expectedPfn)
        {
            LOG_INFO("EPT VERIFICATION PASSED.");
            return true;
        }

        LOG_ERROR("EPT VERIFICATION FAILED.");
        return false;
    }
#endif

    bool Initialize()
    {
        // allocating the root PML4 table
        UINT64 tablePhysicalAddress = 0;
        this->Pml4VirtualAddress = static_cast<PEPT_PML4E>(AllocateEptTable(&tablePhysicalAddress));
        if (this->Pml4VirtualAddress == nullptr)
        {
            LOG_ERROR("Failed to allocate root PML4 table.");
            return false;
        }

        this->Pml4PhysicalAddress = tablePhysicalAddress;

        // allocating the PDPT table (covers 512GB of physical address space)
        PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(AllocateEptTable(&tablePhysicalAddress));
        if (pdptTable == nullptr)
        {
            LOG_ERROR("Failed to allocate PDPT table.");
            return false;
        }

        this->Pml4VirtualAddress[0].Fields.PageDirectoryPointerTableAddress = (tablePhysicalAddress >> PFN_SIZE_BITS);
        this->Pml4VirtualAddress[0].Fields.ReadAccess = 1;
        this->Pml4VirtualAddress[0].Fields.WriteAccess = 1;
        this->Pml4VirtualAddress[0].Fields.ExecuteAccess = 1;

        // allocating 512 PD tables (each holds 512 2MB pages = 1GB per PD)
        for (size_t pdptIndex = 0; pdptIndex < MAX_ENTRY_COUNT; ++pdptIndex)
        {
            PEPT_PDE_2MB pdTable = static_cast<PEPT_PDE_2MB>(AllocateEptTable(&tablePhysicalAddress));
            if (pdTable == nullptr)
            {
                LOG_ERROR("Failed to allocate PD table %zu.", pdptIndex);
                return false;
            }

            pdptTable[pdptIndex].Fields.PageDirectoryAddress = (tablePhysicalAddress >> PFN_SIZE_BITS);
            pdptTable[pdptIndex].Fields.ReadAccess = 1;
            pdptTable[pdptIndex].Fields.WriteAccess = 1;
            pdptTable[pdptIndex].Fields.ExecuteAccess = 1;

            // filling the PD table with 2MB large pages mapping MMIO by default
            for (size_t pdIndex = 0; pdIndex < MAX_ENTRY_COUNT; ++pdIndex)
            {
                pdTable[pdIndex].Fields.ReadAccess = 1;
                pdTable[pdIndex].Fields.WriteAccess = 1;
                pdTable[pdIndex].Fields.ExecuteAccess = 1;
                pdTable[pdIndex].Fields.LargePage = 1;
                pdTable[pdIndex].Fields.EPTMemoryType = 0; // 0 = uncacheable (safe for MMIO)
                pdTable[pdIndex].Fields.PageAddress = (pdptIndex * MAX_ENTRY_COUNT) + pdIndex;
            }

            // shattering the very first 2MB page to protect VGA memory
            if (pdptIndex == 0)
            {
                PEPT_PTE ptTable = static_cast<PEPT_PTE>(AllocateEptTable(&tablePhysicalAddress));
                if (ptTable == nullptr)
                {
                    LOG_ERROR("Failed to allocate PT table for first 2MB.");
                    return false;
                }

                // change PDE 0 from a large page to a standard directory pointer
                PEPT_PDE standardPd = reinterpret_cast<PEPT_PDE>(pdTable);
                standardPd[0].All = 0;
                standardPd[0].Fields.PageTableAddress = (tablePhysicalAddress >> PFN_SIZE_BITS);
                standardPd[0].Fields.ReadAccess = 1;
                standardPd[0].Fields.WriteAccess = 1;
                standardPd[0].Fields.ExecuteAccess = 1;

                // map the 512 4KB pages
                for (size_t ptIndex = 0; ptIndex < MAX_ENTRY_COUNT; ++ptIndex)
                {
                    ptTable[ptIndex].Fields.ReadAccess = 1;
                    ptTable[ptIndex].Fields.WriteAccess = 1;
                    ptTable[ptIndex].Fields.ExecuteAccess = 1;
                    ptTable[ptIndex].Fields.PageAddress = ptIndex;

                    // 0xA0000 to 0xBFFFF is VGA memory
                    // 0xC0000 to 0xFFFFF is BIOS/ROM
                    if (ptIndex >= 0xA0 && ptIndex <= 0xFF)
                    {
                        ptTable[ptIndex].Fields.EPTMemoryType = 0; // uncacheable
                    }
                    else
                    {
                        ptTable[ptIndex].Fields.EPTMemoryType = MEMORY_TYPE_WRITEBACK;
                    }
                }
            }
        }

        // getting the motherboard's RAM boundaries
        PPHYSICAL_MEMORY_RANGE memoryRanges = MmGetPhysicalMemoryRanges();
        if (memoryRanges == nullptr)
        {
            LOG_ERROR("MmGetPhysicalMemoryRanges returned NULL.");
            return false;
        }

        // iterating through the memory ranges to flip specific 2MB pages to write-back
        for (size_t i = 0; memoryRanges[i].NumberOfBytes.QuadPart != 0; ++i)
        {
            UINT64 startAddress = memoryRanges[i].BaseAddress.QuadPart;
            UINT64 endAddress = startAddress + memoryRanges[i].NumberOfBytes.QuadPart;

            UINT64 startPfn2MB = startAddress / (2ull * 1024 * 1024);
            UINT64 endPfn2MB = endAddress / (2ull * 1024 * 1024);

            for (UINT64 pfn = startPfn2MB; pfn <= endPfn2MB; ++pfn)
            {
                if (pfn == 0)
                {
                    continue; // skip first 2MB since it was handled manually
                }

                if (pfn >= (512 * 512)) // 262144
                {
                    continue; // skip mapping beyond 512GB
                }

                UINT64 pdptOffset = (pfn >> OFFSET_SIZE_BITS) & ((1ull << OFFSET_SIZE_BITS) - 1);
                UINT64 pdOffset = pfn & ((1ull << OFFSET_SIZE_BITS) - 1);

                PHYSICAL_ADDRESS pdPhysicalAddress;
                pdPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdptTable[pdptOffset].Fields.PageDirectoryAddress)) << PFN_SIZE_BITS;
                PEPT_PDE_2MB pdTable = static_cast<PEPT_PDE_2MB>(MmGetVirtualForPhysical(pdPhysicalAddress));

                if (pdTable != nullptr)
                {
                    pdTable[pdOffset].Fields.EPTMemoryType = MEMORY_TYPE_WRITEBACK;
                }
            }
        }

        ExFreePool(memoryRanges);

        // build the entry itself
        this->EptPointer.Fields.MemoryType = MEMORY_TYPE_WRITEBACK;
        this->EptPointer.Fields.PageWalkLength = EPT_PAGE_WALK_LENGTH_4;
        this->EptPointer.Fields.DirtyAndAceessEnabled = 1;
        this->EptPointer.Fields.PageMapLevel4Address = (this->Pml4PhysicalAddress >> PFN_SIZE_BITS);

#if DBG
        // if (!VerifyEptMapping()) { return false; }
#endif

        return true;
    }

    void Teardown()
    {
        if (this->Pml4VirtualAddress == nullptr)
        {
            return;
        }

        // level 4 (PML4) loop
        for (size_t pml4Index = 0; pml4Index < MAX_ENTRY_COUNT; ++pml4Index)
        {
            PEPT_PML4E pml4Entry = &this->Pml4VirtualAddress[pml4Index];
            if (pml4Entry->Fields.ReadAccess == 1)
            {
                PHYSICAL_ADDRESS pdptPhysicalAddress;
                pdptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pml4Entry->Fields.PageDirectoryPointerTableAddress)) << PFN_SIZE_BITS;
                PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(MmGetVirtualForPhysical(pdptPhysicalAddress));

                if (pdptTable != nullptr)
                {
                    // level 3 (PDPT) loop
                    for (size_t pdptIndex = 0; pdptIndex < MAX_ENTRY_COUNT; ++pdptIndex)
                    {
                        PEPT_PDPTE pdptEntry = &pdptTable[pdptIndex];
                        if (pdptEntry->Fields.ReadAccess == 1)
                        {
                            PHYSICAL_ADDRESS pdPhysicalAddress;
                            pdPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdptEntry->Fields.PageDirectoryAddress)) << PFN_SIZE_BITS;
                            PEPT_PDE pdTable = static_cast<PEPT_PDE>(MmGetVirtualForPhysical(pdPhysicalAddress));

                            if (pdTable != nullptr)
                            {
                                /// level 2 (PD) loop - check for the shattered 4KB table in the first 2MB
                                if (pdptIndex == 0)
                                {
                                    // cast to the 2MB struct specifically to read the LargePage bit
                                    PEPT_PDE_2MB checkPd = reinterpret_cast<PEPT_PDE_2MB>(pdTable);

                                    // ensuring it's not a large page (LargePage == 0 means it points to a PT)
                                    if (checkPd[0].Fields.ReadAccess == 1 && checkPd[0].Fields.LargePage == 0)
                                    {
                                        PHYSICAL_ADDRESS ptPhysicalAddress;
                                        ptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdTable[0].Fields.PageTableAddress)) << PFN_SIZE_BITS;
                                        PEPT_PTE ptTable = static_cast<PEPT_PTE>(MmGetVirtualForPhysical(ptPhysicalAddress));

                                        if (ptTable != nullptr)
                                        {
                                            ExFreePoolWithTag(ptTable, EPT_POOL_TAG);
                                        }
                                    }
                                }

                                ExFreePoolWithTag(pdTable, EPT_POOL_TAG);
                            }
                        }
                    }
                    ExFreePoolWithTag(pdptTable, EPT_POOL_TAG);
                }
            }
        }

        ExFreePoolWithTag(this->Pml4VirtualAddress, EPT_POOL_TAG);

        this->Pml4VirtualAddress = nullptr;
        this->Pml4PhysicalAddress = 0;
        this->EptPointer.All = 0;
    }

    EPT_POINTER GetEptPointer() const
    {
        return this->EptPointer;
    }

private:
    PEPT_PML4E Pml4VirtualAddress;
    UINT64 Pml4PhysicalAddress;
    EPT_POINTER EptPointer;

    // returns the virtual address of the allocated table and outputs its physical address through the output parameter
    PVOID AllocateEptTable(UINT64* outPhysicalAddress)
    {
        PVOID table = ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, EPT_POOL_TAG);
        if (table == nullptr)
        {
            return nullptr;
        }

        *outPhysicalAddress = MmGetPhysicalAddress(table).QuadPart;
        return table;
    }
};
