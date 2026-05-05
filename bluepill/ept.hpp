#pragma once

#include "constants.h"
#include "structs.h"
#include "utils.hpp"
#include <ntddk.h>

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
        if (truePhysicalAddress.QuadPart == 0)
        {
            LOG_ERROR("VERIFIER: MmGetPhysicalAddress failed to translate testVariable.");
            return false;
        }

        UINT64 pml4Offset = (truePhysicalAddress.QuadPart >> EPT_SHIFTS::PML4) & EPT_SHIFTS::INDEX_MASK;
        UINT64 pdptOffset = (truePhysicalAddress.QuadPart >> EPT_SHIFTS::PDPT) & EPT_SHIFTS::INDEX_MASK;
        UINT64 pdOffset = (truePhysicalAddress.QuadPart >> EPT_SHIFTS::PD) & EPT_SHIFTS::INDEX_MASK;
        UINT64 ptOffset = (truePhysicalAddress.QuadPart >> EPT_SHIFTS::PT) & EPT_SHIFTS::INDEX_MASK;

        // walking PML4
        PEPT_PML4E pml4Entry = &this->Pml4VirtualAddress[pml4Offset];
        if (pml4Entry->Fields.ReadAccess == 0)
        {
            LOG_ERROR("VERIFIER: PML4 Walk Failed. Index: %llu is empty.", pml4Offset);
            return false;
        }

        // walking PDPT
        PHYSICAL_ADDRESS pdptPhysicalAddress;
        pdptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pml4Entry->Fields.PageDirectoryPointerTableAddress)) << PAGE_SHIFT;
        PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(MmGetVirtualForPhysical(pdptPhysicalAddress));
        if (!pdptTable)
        {
            LOG_ERROR("VERIFIER: MmGetVirtualForPhysical failed on PDPT Physical Address: 0x%llX", pdptPhysicalAddress.QuadPart);
            return false;
        }
        if (pdptTable[pdptOffset].Fields.ReadAccess == 0)
        {
            LOG_ERROR("VERIFIER: PDPT Walk Failed. Index: %llu is empty.", pdptOffset);
            return false;
        }

        // walking PD
        PHYSICAL_ADDRESS pdPhysicalAddress;
        pdPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdptTable[pdptOffset].Fields.PageDirectoryAddress)) << PAGE_SHIFT;
        PEPT_PDE pdTable = static_cast<PEPT_PDE>(MmGetVirtualForPhysical(pdPhysicalAddress));
        if (!pdTable)
        {
            LOG_ERROR("VERIFIER: MmGetVirtualForPhysical failed on PD Physical Address: 0x%llX", pdPhysicalAddress.QuadPart);
            return false;
        }
        if (pdTable[pdOffset].Fields.ReadAccess == 0)
        {
            LOG_ERROR("VERIFIER: PD Walk Failed. Index: %llu is empty.", pdOffset);
            return false;
        }

        // checking if this is a 2MB Large Page
        PEPT_PDE_2MB pdTableLarge = reinterpret_cast<PEPT_PDE_2MB>(pdTable);
        if (pdTableLarge[pdOffset].Fields.LargePage == 1)
        {
            UINT64 expected2MbPfn = truePhysicalAddress.QuadPart / EPT_CONFIG::SIZE_2MB;
            if (pdTableLarge[pdOffset].Fields.PageAddress == expected2MbPfn)
            {
                LOG_INFO("EPT VERIFICATION PASSED (2MB Large Page).");
                return true;
            }
            LOG_ERROR("VERIFIER: 2MB PFN Mismatch. Expected: %llu, Got: %llu", expected2MbPfn, (UINT64)pdTableLarge[pdOffset].Fields.PageAddress);
            return false;
        }

        // walking PT (only if the PD pointed to a standard 4KB table)
        PHYSICAL_ADDRESS ptPhysicalAddress;
        ptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdTable[pdOffset].Fields.PageTableAddress)) << PAGE_SHIFT;
        PEPT_PTE ptTable = static_cast<PEPT_PTE>(MmGetVirtualForPhysical(ptPhysicalAddress));
        if (!ptTable)
        {
            LOG_ERROR("VERIFIER: MmGetVirtualForPhysical failed on PT Physical Address: 0x%llX", ptPhysicalAddress.QuadPart);
            return false;
        }
        if (ptTable[ptOffset].Fields.ReadAccess == 0)
        {
            LOG_ERROR("VERIFIER: PT Walk Failed. Index: %llu is empty.", ptOffset);
            return false;
        }

        UINT64 expected4KbPfn = truePhysicalAddress.QuadPart >> PAGE_SHIFT;
        if (ptTable[ptOffset].Fields.PageAddress == expected4KbPfn)
        {
            LOG_INFO("EPT VERIFICATION PASSED (4KB Page).");
            return true;
        }

        LOG_ERROR("VERIFIER: 4KB PFN Mismatch. Expected: %llu, Got: %llu", expected4KbPfn, (UINT64)ptTable[ptOffset].Fields.PageAddress);
        return false;
    }
#endif

    bool Initialize()
    {
        UINT64 tablePhysicalAddress = 0;

        // allocating PML4
        this->Pml4VirtualAddress = static_cast<PEPT_PML4E>(AllocateEptTable(&tablePhysicalAddress));
        if (!this->Pml4VirtualAddress)
        {
            LOG_ERROR("EPT INIT FAILED: Could not allocate PML4 table.");
            return false;
        }
        this->Pml4PhysicalAddress = tablePhysicalAddress;

        // allocating PDPT
        PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(AllocateEptTable(&tablePhysicalAddress));
        if (!pdptTable)
        {
            LOG_ERROR("EPT INIT FAILED: Could not allocate PDPT table.");
            return false;
        }

        this->Pml4VirtualAddress[0].Fields.PageDirectoryPointerTableAddress = (tablePhysicalAddress >> PAGE_SHIFT);
        this->Pml4VirtualAddress[0].Fields.ReadAccess = 1;
        this->Pml4VirtualAddress[0].Fields.WriteAccess = 1;
        this->Pml4VirtualAddress[0].Fields.ExecuteAccess = 1;

        // allocating PD Tables (mapping 512GB of RAM)
        for (size_t pdptIndex = 0; pdptIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++pdptIndex)
        {
            PEPT_PDE_2MB pdTable = static_cast<PEPT_PDE_2MB>(AllocateEptTable(&tablePhysicalAddress));
            if (!pdTable)
            {
                LOG_ERROR("EPT INIT FAILED: Could not allocate PD table at index %zu.", pdptIndex);
                return false;
            }

            pdptTable[pdptIndex].Fields.PageDirectoryAddress = (tablePhysicalAddress >> PAGE_SHIFT);
            pdptTable[pdptIndex].Fields.ReadAccess = 1;
            pdptTable[pdptIndex].Fields.WriteAccess = 1;
            pdptTable[pdptIndex].Fields.ExecuteAccess = 1;

            // pre filling as 2MB large pages (uncacheable mmio by default)
            for (size_t pdIndex = 0; pdIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++pdIndex)
            {
                pdTable[pdIndex].Fields.ReadAccess = 1;
                pdTable[pdIndex].Fields.WriteAccess = 1;
                pdTable[pdIndex].Fields.ExecuteAccess = 1;
                pdTable[pdIndex].Fields.LargePage = 1;
                pdTable[pdIndex].Fields.EPTMemoryType = MEMORY_TYPES::UNCACHEABLE;
                pdTable[pdIndex].Fields.PageAddress = (pdptIndex * EPT_CONFIG::MAX_ENTRY_COUNT) + pdIndex;
            }

            // shattering the first 2MB block into 4KB pages to handle VGA/BIOS
            if (pdptIndex == 0)
            {
                PEPT_PTE ptTable = static_cast<PEPT_PTE>(AllocateEptTable(&tablePhysicalAddress));
                if (!ptTable)
                {
                    LOG_ERROR("EPT INIT FAILED: Could not allocate PT table for shattering the first 2MB.");
                    return false;
                }

                PEPT_PDE standardPd = reinterpret_cast<PEPT_PDE>(pdTable);
                standardPd[0].All = 0;
                standardPd[0].Fields.PageTableAddress = (tablePhysicalAddress >> PAGE_SHIFT);
                standardPd[0].Fields.ReadAccess = 1;
                standardPd[0].Fields.WriteAccess = 1;
                standardPd[0].Fields.ExecuteAccess = 1;

                for (size_t ptIndex = 0; ptIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++ptIndex)
                {
                    ptTable[ptIndex].Fields.ReadAccess = 1;
                    ptTable[ptIndex].Fields.WriteAccess = 1;
                    ptTable[ptIndex].Fields.ExecuteAccess = 1;
                    ptTable[ptIndex].Fields.PageAddress = ptIndex;

                    if (ptIndex >= EPT_CONFIG::VGA_MEMORY_START_PFN && ptIndex <= EPT_CONFIG::BIOS_MEMORY_END_PFN)
                    {
                        ptTable[ptIndex].Fields.EPTMemoryType = MEMORY_TYPES::UNCACHEABLE;
                    }
                    else
                    {
                        ptTable[ptIndex].Fields.EPTMemoryType = MEMORY_TYPES::WRITEBACK;
                    }
                }
            }
        }

        // querying windows for valid RAM and marking those 2MB pages as write back
        PPHYSICAL_MEMORY_RANGE memoryRanges = MmGetPhysicalMemoryRanges();
        if (!memoryRanges)
        {
            LOG_ERROR("EPT INIT FAILED: MmGetPhysicalMemoryRanges returned NULL.");
            return false;
        }

        for (size_t i = 0; memoryRanges[i].NumberOfBytes.QuadPart != 0; ++i)
        {
            UINT64 startAddress = memoryRanges[i].BaseAddress.QuadPart;
            UINT64 endAddress = startAddress + memoryRanges[i].NumberOfBytes.QuadPart;

            UINT64 startPfn2MB = startAddress / EPT_CONFIG::SIZE_2MB;
            UINT64 endPfn2MB = endAddress / EPT_CONFIG::SIZE_2MB;

            for (UINT64 pfn = startPfn2MB; pfn <= endPfn2MB; ++pfn)
            {
                if (pfn == 0 || pfn >= (EPT_CONFIG::MAX_ENTRY_COUNT * EPT_CONFIG::MAX_ENTRY_COUNT))
                {
                    continue;
                }

                UINT64 pdptOffset = (pfn >> EPT_SHIFTS::BITS_PER_LEVEL) & EPT_SHIFTS::INDEX_MASK;
                UINT64 pdOffset = pfn & EPT_SHIFTS::INDEX_MASK;

                PHYSICAL_ADDRESS pdPhysicalAddress;
                pdPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdptTable[pdptOffset].Fields.PageDirectoryAddress)) << PAGE_SHIFT;
                PEPT_PDE_2MB pdTable = static_cast<PEPT_PDE_2MB>(MmGetVirtualForPhysical(pdPhysicalAddress));

                if (pdTable)
                {
                    pdTable[pdOffset].Fields.EPTMemoryType = MEMORY_TYPES::WRITEBACK;
                }
            }
        }

        ExFreePool(memoryRanges);

        // building final EPT pointer
        this->EptPointer.Fields.MemoryType = MEMORY_TYPES::WRITEBACK;
        this->EptPointer.Fields.PageWalkLength = EPT_CONFIG::PAGE_WALK_LENGTH_4;
        this->EptPointer.Fields.DirtyAndAceessEnabled = 1;
        this->EptPointer.Fields.PageMapLevel4Address = (this->Pml4PhysicalAddress >> PAGE_SHIFT);

        // forcing the cpu to commit all page table writes to the physical RAM
        // this is required for the hardware page walker before vmlaunch
        KeMemoryBarrier();

#if DBG
        if (!VerifyEptMapping())
        {
            LOG_ERROR("EPT INIT FAILED: VerifyEptMapping returned false.");
            return false;
        }
#endif

        return true;
    }

    void Teardown()
    {
        if (!this->Pml4VirtualAddress)
        {
            return;
        }

        for (size_t pml4Index = 0; pml4Index < EPT_CONFIG::MAX_ENTRY_COUNT; ++pml4Index)
        {
            PEPT_PML4E pml4Entry = &this->Pml4VirtualAddress[pml4Index];
            if (pml4Entry->Fields.ReadAccess == 1)
            {
                PHYSICAL_ADDRESS pdptPhysicalAddress;
                pdptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pml4Entry->Fields.PageDirectoryPointerTableAddress)) << PAGE_SHIFT;
                PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(MmGetVirtualForPhysical(pdptPhysicalAddress));

                if (pdptTable)
                {
                    for (size_t pdptIndex = 0; pdptIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++pdptIndex)
                    {
                        PEPT_PDPTE pdptEntry = &pdptTable[pdptIndex];
                        if (pdptEntry->Fields.ReadAccess == 1)
                        {
                            PHYSICAL_ADDRESS pdPhysicalAddress;
                            pdPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdptEntry->Fields.PageDirectoryAddress)) << PAGE_SHIFT;
                            PEPT_PDE pdTable = static_cast<PEPT_PDE>(MmGetVirtualForPhysical(pdPhysicalAddress));

                            if (pdTable)
                            {
                                if (pdptIndex == 0)
                                {
                                    PEPT_PDE_2MB checkPd = reinterpret_cast<PEPT_PDE_2MB>(pdTable);
                                    if (checkPd[0].Fields.ReadAccess == 1 && checkPd[0].Fields.LargePage == 0)
                                    {
                                        PHYSICAL_ADDRESS ptPhysicalAddress;
                                        ptPhysicalAddress.QuadPart = (static_cast<ULONG64>(pdTable[0].Fields.PageTableAddress)) << PAGE_SHIFT;
                                        PEPT_PTE ptTable = static_cast<PEPT_PTE>(MmGetVirtualForPhysical(ptPhysicalAddress));
                                        if (ptTable)
                                        {
                                            MmFreeContiguousMemory(ptTable);
                                        }
                                    }
                                }
                                MmFreeContiguousMemory(pdTable);
                            }
                        }
                    }
                    MmFreeContiguousMemory(pdptTable);
                }
            }
        }

        MmFreeContiguousMemory(this->Pml4VirtualAddress);
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

    PVOID AllocateEptTable(UINT64* outPhysicalAddress)
    {
        PHYSICAL_ADDRESS highestAddress;
        highestAddress.QuadPart = ~0ull;

        PVOID table = MmAllocateContiguousMemory(PAGE_SIZE, highestAddress);
        if (table == nullptr)
        {
            return nullptr;
        }

        // CRITICAL: preventing reading garbage memory as valid page table entries
        RtlSecureZeroMemory(table, PAGE_SIZE);

        *outPhysicalAddress = MmGetPhysicalAddress(table).QuadPart;
        return table;
    }
};
