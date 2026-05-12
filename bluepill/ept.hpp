#pragma once

#include "constants.hpp"
#include "structs.hpp"
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
        InitializeListHead(&this->TablesList);
    }

    ~VmmEpt()
    {
        Teardown();
    }

#if DBG
    bool VerifyEptMapping()
    {
        // using a stack variable to pick an arbitrary GPA that should be in writeback RAM
        // since the kernel stack lives in normal RAM, this exercises the writeback path that
        // matters for guest correctness
        volatile ULONG testVariable = 0x1337;

        PHYSICAL_ADDRESS truePhysicalAddress = MmGetPhysicalAddress((PVOID)&testVariable);
        if (truePhysicalAddress.QuadPart == 0)
        {
            LOG_ERROR("VERIFIER: MmGetPhysicalAddress failed to translate testVariable.");
            return false;
        }

        // checking if the GPA is covered by a 2MB large page
        PEPT_PDE_2MB pde2Mb = GetPde2MbForGpa(truePhysicalAddress.QuadPart);
        if (pde2Mb != nullptr)
        {
            UINT64 expected2MbPfn = truePhysicalAddress.QuadPart / EPT_CONFIG::SIZE_2MB;
            if (pde2Mb->Fields.PageAddress != expected2MbPfn)
            {
                LOG_ERROR("VERIFIER: 2MB PFN Mismatch. Expected: %llu, Got: %llu",
                          expected2MbPfn, (UINT64)pde2Mb->Fields.PageAddress);
                return false;
            }
            if (pde2Mb->Fields.EPTMemoryType != MEMORY_TYPES::WRITEBACK)
            {
                LOG_ERROR("VERIFIER: 2MB page for stack GPA is not WB (got %llu).",
                          (UINT64)pde2Mb->Fields.EPTMemoryType);
                return false;
            }
            LOG_INFO("EPT VERIFICATION PASSED (2MB Large Page, WB).");
            return true;
        }

        // otherwise it should be a 4KB mapping
        PEPT_PTE pte = GetPteForGpa(truePhysicalAddress.QuadPart);
        if (pte == nullptr)
        {
            LOG_ERROR("VERIFIER: GPA 0x%llX has neither a 2MB nor 4KB mapping.",
                      truePhysicalAddress.QuadPart);
            return false;
        }

        UINT64 expected4KbPfn = truePhysicalAddress.QuadPart >> PAGE_SHIFT;
        if (pte->Fields.PageAddress != expected4KbPfn)
        {
            LOG_ERROR("VERIFIER: 4KB PFN Mismatch. Expected: %llu, Got: %llu",
                      expected4KbPfn, (UINT64)pte->Fields.PageAddress);
            return false;
        }
        if (pte->Fields.EPTMemoryType != MEMORY_TYPES::WRITEBACK)
        {
            LOG_ERROR("VERIFIER: 4KB page for stack GPA is not WB (got %llu).",
                      (UINT64)pte->Fields.EPTMemoryType);
            return false;
        }
        LOG_INFO("EPT VERIFICATION PASSED (4KB Page, WB).");
        return true;
    }
#endif

    bool Initialize()
    {
        UINT64 tablePhysicalAddress = 0;

        // allocating PML4. note we do NOT add the PML4 to TablesList since we hold
        // a direct pointer to it and free it separately in Teardown.
        this->Pml4VirtualAddress = static_cast<PEPT_PML4E>(AllocateUntrackedTable(&tablePhysicalAddress));
        if (!this->Pml4VirtualAddress)
        {
            LOG_ERROR("EPT INIT FAILED: Could not allocate PML4 table.");
            return false;
        }
        this->Pml4PhysicalAddress = tablePhysicalAddress;

        // allocating PDPT
        PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(AllocateTrackedTable(&tablePhysicalAddress));
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
        for (UINT64 pdptIndex = 0; pdptIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++pdptIndex)
        {
            PEPT_PDE_2MB pdTable = static_cast<PEPT_PDE_2MB>(AllocateTrackedTable(&tablePhysicalAddress));
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
            for (UINT64 pdIndex = 0; pdIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++pdIndex)
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
                PEPT_PTE ptTable = static_cast<PEPT_PTE>(AllocateTrackedTable(&tablePhysicalAddress));
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

                for (UINT64 ptIndex = 0; ptIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++ptIndex)
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

        for (UINT64 i = 0; memoryRanges[i].NumberOfBytes.QuadPart != 0; ++i)
        {
            UINT64 startAddress = memoryRanges[i].BaseAddress.QuadPart;
            UINT64 endAddress = startAddress + memoryRanges[i].NumberOfBytes.QuadPart;

            // ranges might end mid 2MB page so we need to round up the exclusive end
            UINT64 startPfn2MB = startAddress / EPT_CONFIG::SIZE_2MB;
            UINT64 endPfn2MBExclusive = (endAddress + EPT_CONFIG::SIZE_2MB - 1) / EPT_CONFIG::SIZE_2MB;

            for (UINT64 pfn = startPfn2MB; pfn < endPfn2MBExclusive; ++pfn)
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
        // freeing every dynamically allocated table and runtime splits
        // since all of them are tracked in TablesList there's no tree walk needed.
        while (!IsListEmpty(&this->TablesList))
        {
            PLIST_ENTRY entry = RemoveHeadList(&this->TablesList);
            EptTableNode* node = CONTAINING_RECORD(entry, EptTableNode, ListEntry);
            if (node->TableVa)
            {
                MmFreeContiguousMemory(node->TableVa);
            }
            ExFreePoolWithTag(node, EPT_CONFIG::POOL_TAG);
        }

        if (this->Pml4VirtualAddress)
        {
            MmFreeContiguousMemory(this->Pml4VirtualAddress);
            this->Pml4VirtualAddress = nullptr;
        }

        this->Pml4PhysicalAddress = 0;
        this->EptPointer.All = 0;
    }

    EPT_POINTER GetEptPointer() const
    {
        return this->EptPointer;
    }

    // returns the 4KB PTE that maps this GPA, or null if the GPA is covered by a
    // 2MB large page (the caller needs to call SplitLargePage first) or the walk fails
    PEPT_PTE GetPteForGpa(UINT64 gpa)
    {
        PEPT_PDE pd = nullptr;
        UINT64 pdIndex = 0;
        if (!WalkToPd(gpa, &pd, &pdIndex))
        {
            return nullptr;
        }

        // if it's still a large page, there's no PT to return
        PEPT_PDE_2MB pdLarge = reinterpret_cast<PEPT_PDE_2MB>(pd);
        if (pdLarge[pdIndex].Fields.LargePage == 1)
        {
            return nullptr;
        }

        PHYSICAL_ADDRESS ptPhysical;
        ptPhysical.QuadPart = (static_cast<ULONG64>(pd[pdIndex].Fields.PageTableAddress)) << PAGE_SHIFT;
        PEPT_PTE ptTable = static_cast<PEPT_PTE>(MmGetVirtualForPhysical(ptPhysical));
        if (!ptTable)
        {
            return nullptr;
        }

        UINT64 ptIndex = (gpa >> EPT_SHIFTS::PT) & EPT_SHIFTS::INDEX_MASK;
        return &ptTable[ptIndex];
    }

    // returns the 2MB PDE that maps this GPA, or null if the GPA is split into 4KB
    // pages or the walk fails
    PEPT_PDE_2MB GetPde2MbForGpa(UINT64 gpa)
    {
        PEPT_PDE pd = nullptr;
        UINT64 pdIndex = 0;
        if (!WalkToPd(gpa, &pd, &pdIndex))
        {
            return nullptr;
        }

        PEPT_PDE_2MB pdLarge = reinterpret_cast<PEPT_PDE_2MB>(pd);
        if (pdLarge[pdIndex].Fields.LargePage == 0)
        {
            return nullptr;
        }

        return &pdLarge[pdIndex];
    }

    // splits a 2MB large page covering this GPA into 512 individual 4KB pages,
    // preserving permissions and memory type. does nothing if the large page is already split.
    //
    // this allocates from non paged contiguous memory and must run at
    // PASSIVE_LEVEL. for runtime splitting from our vmexit handler, we need to
    // pre-split during initialization or to maintain a pre-allocated pool of page tables.
    //
    // also, this function does NOT execute INVEPT. the caller is responsible since
    // they know the right scope (all vcpus sharing this EPT need invalidation if we're post launch).
    bool SplitLargePage(UINT64 gpa)
    {
        PEPT_PDE pd = nullptr;
        UINT64 pdIndex = 0;
        if (!WalkToPd(gpa, &pd, &pdIndex))
        {
            LOG_ERROR("SplitLargePage: walk failed for GPA 0x%llX", gpa);
            return false;
        }

        PEPT_PDE_2MB pdLarge = reinterpret_cast<PEPT_PDE_2MB>(pd);

        // already split, no work needed
        if (pdLarge[pdIndex].Fields.LargePage == 0)
        {
            return true;
        }

        // snapshotting the existing 2MB PDE so the new 4KB entries inherit its permissions and memory type
        EPT_PDE_2MB oldPde;
        oldPde.All = pdLarge[pdIndex].All;

        UINT64 ptPhysical = 0;
        PEPT_PTE newPt = static_cast<PEPT_PTE>(AllocateTrackedTable(&ptPhysical));
        if (!newPt)
        {
            LOG_ERROR("SplitLargePage: AllocateTrackedTable failed for GPA 0x%llX", gpa);
            return false;
        }

        UINT64 basePfn = static_cast<UINT64>(oldPde.Fields.PageAddress) * EPT_CONFIG::MAX_ENTRY_COUNT;

        for (UINT64 ptIndex = 0; ptIndex < EPT_CONFIG::MAX_ENTRY_COUNT; ++ptIndex)
        {
            EPT_PTE entry;
            entry.All = 0;
            entry.Fields.ReadAccess = oldPde.Fields.ReadAccess;
            entry.Fields.WriteAccess = oldPde.Fields.WriteAccess;
            entry.Fields.ExecuteAccess = oldPde.Fields.ExecuteAccess;
            entry.Fields.EPTMemoryType = oldPde.Fields.EPTMemoryType;
            entry.Fields.IgnorePAT = oldPde.Fields.IgnorePAT;
            entry.Fields.PageAddress = basePfn + ptIndex;
            newPt[ptIndex].All = entry.All;
        }

        // building the replacement non-leaf PDE pointing at the new PT
        EPT_PDE newPde;
        newPde.All = 0;
        newPde.Fields.ReadAccess = 1;
        newPde.Fields.WriteAccess = 1;
        newPde.Fields.ExecuteAccess = 1;
        newPde.Fields.PageTableAddress = ptPhysical >> PAGE_SHIFT;

        // making sure the page table writes are globally visible before the PDE swap so a
        // concurrent CPU walker doesn't see a PDE pointing to a partially built page table
        KeMemoryBarrier();

        // making sure the hardware walker doesn't read a torn value with an atomic write
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&pdLarge[pdIndex].All),
                              static_cast<LONG64>(newPde.All));

        return true;
    }

    // for convenience: split if needed, then return the 4KB PTE for this GPA. this is
    // what hook install code typically needs
    PEPT_PTE GetOrSplitPte(UINT64 gpa)
    {
        if (!SplitLargePage(gpa))
        {
            return nullptr;
        }
        return GetPteForGpa(gpa);
    }

private:
    struct EptTableNode
    {
        LIST_ENTRY ListEntry;
        PVOID TableVa;
    };

    PEPT_PML4E Pml4VirtualAddress;
    UINT64 Pml4PhysicalAddress;
    EPT_POINTER EptPointer;
    LIST_ENTRY TablesList;

    PVOID AllocateUntrackedTable(UINT64* outPhysicalAddress)
    {
        PHYSICAL_ADDRESS highestAddress;
        highestAddress.QuadPart = ~0ull;

        PVOID table = MmAllocateContiguousMemory(PAGE_SIZE, highestAddress);
        if (table == nullptr)
        {
            return nullptr;
        }

        // preventing reading garbage memory as valid page table entries
        RtlSecureZeroMemory(table, PAGE_SIZE);

        *outPhysicalAddress = MmGetPhysicalAddress(table).QuadPart;
        return table;
    }

    // allocates a page table and tracks it in TablesList so Teardown can free it
    // without walking the tree. returns null and undoes the allocation on tracker
    // failure.
    PVOID AllocateTrackedTable(UINT64* outPhysicalAddress)
    {
        PVOID table = AllocateUntrackedTable(outPhysicalAddress);
        if (!table)
        {
            return nullptr;
        }

        EptTableNode* node = static_cast<EptTableNode*>(
            ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(EptTableNode), EPT_CONFIG::POOL_TAG));
        if (!node)
        {
            MmFreeContiguousMemory(table);
            return nullptr;
        }

        node->TableVa = table;
        InsertTailList(&this->TablesList, &node->ListEntry);

        return table;
    }

    // walks PML4 -> PDPT -> PD and returns a pointer to the PD table plus the
    // index of the entry that maps this GPA. the caller decides whether to interpret
    // the entry as a 2MB leaf or a 4KB PDE.
    bool WalkToPd(UINT64 gpa, PEPT_PDE* outPd, UINT64* outPdIndex)
    {
        UINT64 pml4Index = (gpa >> EPT_SHIFTS::PML4) & EPT_SHIFTS::INDEX_MASK;
        UINT64 pdptIndex = (gpa >> EPT_SHIFTS::PDPT) & EPT_SHIFTS::INDEX_MASK;
        UINT64 pdIndex = (gpa >> EPT_SHIFTS::PD) & EPT_SHIFTS::INDEX_MASK;

        if (!this->Pml4VirtualAddress)
        {
            return false;
        }

        PEPT_PML4E pml4Entry = &this->Pml4VirtualAddress[pml4Index];
        if (pml4Entry->Fields.ReadAccess == 0)
        {
            return false;
        }

        PHYSICAL_ADDRESS pdptPhysical;
        pdptPhysical.QuadPart = (static_cast<ULONG64>(pml4Entry->Fields.PageDirectoryPointerTableAddress)) << PAGE_SHIFT;
        PEPT_PDPTE pdptTable = static_cast<PEPT_PDPTE>(MmGetVirtualForPhysical(pdptPhysical));
        if (!pdptTable || pdptTable[pdptIndex].Fields.ReadAccess == 0)
        {
            return false;
        }

        PHYSICAL_ADDRESS pdPhysical;
        pdPhysical.QuadPart = (static_cast<ULONG64>(pdptTable[pdptIndex].Fields.PageDirectoryAddress)) << PAGE_SHIFT;
        PEPT_PDE pdTable = static_cast<PEPT_PDE>(MmGetVirtualForPhysical(pdPhysical));
        if (!pdTable)
        {
            return false;
        }

        *outPd = pdTable;
        *outPdIndex = pdIndex;
        return true;
    }
};
