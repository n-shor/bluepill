#pragma once

#include "structs.h"
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
            DbgPrint("[+] EPT VERIFICATION PASSED.\n");
            return true;
        }

        DbgPrint("[-] EPT VERIFICATION FAILED.\n");
        return false;
    }
#endif

    bool Initialize()
    {
        // allocating the root PML4 table
        this->Pml4VirtualAddress = static_cast<PEPT_PML4E>(
            ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, EPT_POOL_TAG));
        if (this->Pml4VirtualAddress == nullptr)
        {
            DbgPrint("[-] ERROR: Failed to allocate root PML4 table.\n");
            return false;
        }

        this->Pml4PhysicalAddress = MmGetPhysicalAddress(this->Pml4VirtualAddress).QuadPart;

        // getting the motherboard's RAM boundaries
        PPHYSICAL_MEMORY_RANGE memoryRanges = MmGetPhysicalMemoryRanges();
        if (memoryRanges == nullptr)
        {
            DbgPrint("[-] ERROR: MmGetPhysicalMemoryRanges returned NULL.\n");
            return false;
        }

        // cache variables to increase initialization speed and to prevent lagspike induced crashes
        UINT64 cachedPml4Offset = ~0ULL;
        PEPT_PDPTE currentPdptTable = nullptr;

        UINT64 cachedPdptOffset = ~0ULL;
        PEPT_PDE currentPdTable = nullptr;

        UINT64 cachedPdOffset = ~0ULL;
        PEPT_PTE currentPtTable = nullptr;

        // iterating through the memory ranges
        for (size_t i = 0; memoryRanges[i].NumberOfBytes.QuadPart != 0; ++i)
        {
            UINT64 startAddress = memoryRanges[i].BaseAddress.QuadPart;
            UINT64 endAddress = startAddress + memoryRanges[i].NumberOfBytes.QuadPart;

            // going across the RAM 4KB at a time
            for (UINT64 physicalAddress = startAddress; physicalAddress < endAddress; physicalAddress += PAGE_SIZE)
            {
                UINT64 pml4Offset = (physicalAddress >> (ADDRESS_SIZE_BITS - OFFSET_SIZE_BITS)) & ((1ull << OFFSET_SIZE_BITS) - 1);
                UINT64 pdptOffset = (physicalAddress >> (ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 2))) & ((1ull << OFFSET_SIZE_BITS) - 1);
                UINT64 pdOffset = (physicalAddress >> (ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 3))) & ((1ull << OFFSET_SIZE_BITS) - 1);
                UINT64 ptOffset = (physicalAddress >> (ADDRESS_SIZE_BITS - (OFFSET_SIZE_BITS * 4))) & ((1ull << OFFSET_SIZE_BITS) - 1);

                // --- LEVEL 4 (PML4) ---
                PEPT_PML4E pml4Entry = &this->Pml4VirtualAddress[pml4Offset];

                if (pml4Offset != cachedPml4Offset)
                {
                    UINT64 tablePhysicalAddress = 0;
                    currentPdptTable = static_cast<PEPT_PDPTE>(AllocateEptTable(&tablePhysicalAddress));
                    if (currentPdptTable == nullptr)
                    {
                        DbgPrint("[-] ERROR: Failed to allocate PDPT Table.\n");
                        return false;
                    }

                    pml4Entry->Fields.PageDirectoryPointerTableAddress = (tablePhysicalAddress >> PFN_SIZE_BITS);
                    pml4Entry->Fields.ReadAccess = 1;
                    pml4Entry->Fields.WriteAccess = 1;
                    pml4Entry->Fields.ExecuteAccess = 1;

                    cachedPml4Offset = pml4Offset;
                }

                // --- LEVEL 3 (PDPT) ---
                PEPT_PDPTE pdptEntry = &currentPdptTable[pdptOffset];

                if (pdptOffset != cachedPdptOffset)
                {
                    UINT64 tablePhysicalAddress = 0;
                    currentPdTable = static_cast<PEPT_PDE>(AllocateEptTable(&tablePhysicalAddress));
                    if (currentPdTable == nullptr)
                    {
                        DbgPrint("[-] ERROR: Failed to allocate PD Table.\n");
                        return false;
                    }

                    pdptEntry->Fields.PageDirectoryAddress = (tablePhysicalAddress >> PFN_SIZE_BITS);
                    pdptEntry->Fields.ReadAccess = 1;
                    pdptEntry->Fields.WriteAccess = 1;
                    pdptEntry->Fields.ExecuteAccess = 1;

                    cachedPdptOffset = pdptOffset;
                }

                // --- LEVEL 2 (PD) ---
                PEPT_PDE pdEntry = &currentPdTable[pdOffset];

                if (pdOffset != cachedPdOffset)
                {
                    UINT64 tablePhysicalAddress = 0;
                    currentPtTable = static_cast<PEPT_PTE>(AllocateEptTable(&tablePhysicalAddress));
                    if (currentPtTable == nullptr)
                    {
                        DbgPrint("[-] ERROR: Failed to allocate PT Table.\n");
                        return false;
                    }

                    pdEntry->Fields.PageTableAddress = (tablePhysicalAddress >> PFN_SIZE_BITS);
                    pdEntry->Fields.ReadAccess = 1;
                    pdEntry->Fields.WriteAccess = 1;
                    pdEntry->Fields.ExecuteAccess = 1;

                    cachedPdOffset = pdOffset;
                }

                // --- LEVEL 1 (PT) ---
                PEPT_PTE ptEntry = &currentPtTable[ptOffset];

                // map the current loop's physical address directly
                ptEntry->Fields.PageAddress = (physicalAddress >> PFN_SIZE_BITS);
                ptEntry->Fields.ReadAccess = 1;
                ptEntry->Fields.WriteAccess = 1;
                ptEntry->Fields.ExecuteAccess = 1;
                ptEntry->Fields.EPTMemoryType = MEMORY_TYPE_WRITEBACK;
            }
        }

        ExFreePool(memoryRanges);

        // build the entry itself
        this->EptPointer.Fields.MemoryType = MEMORY_TYPE_WRITEBACK;
        this->EptPointer.Fields.PageWalkLength = EPT_PAGE_WALK_LENGTH_4;
        this->EptPointer.Fields.DirtyAndAceessEnabled = 1;
        this->EptPointer.Fields.PageMapLevel4Address = (this->Pml4PhysicalAddress >> PFN_SIZE_BITS);

#if DBG
        if (!VerifyEptMapping())
        {
            return false;
        }
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
                pdptPhysicalAddress.QuadPart = (static_cast<ULONG64>(
                                                   pml4Entry->Fields.PageDirectoryPointerTableAddress))
                                               << PFN_SIZE_BITS;
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
                                // level 2 (PD) loop
                                for (size_t pdIndex = 0; pdIndex < MAX_ENTRY_COUNT; ++pdIndex)
                                {
                                    PEPT_PDE pdEntry = &pdTable[pdIndex];
                                    if (pdEntry->Fields.ReadAccess == 1)
                                    {
                                        PHYSICAL_ADDRESS ptPhysicalAddress;
                                        ptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdEntry->Fields.PageTableAddress)) << PFN_SIZE_BITS;
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
