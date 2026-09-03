#pragma once

#include "constants.hpp"
#include "structs.hpp"
#include "utils.hpp"
#include <intrin.h>
#include <ntifs.h>

class ContiguousMemory
{
private:
    PVOID m_virtualAddress;
    unsigned long long m_physicalAddress;

    ContiguousMemory(PVOID virtualAddress, unsigned long long physicalAddress)
        : m_virtualAddress(virtualAddress), m_physicalAddress(physicalAddress)
    {
    }

    void Reset() noexcept
    {
        if (m_virtualAddress != nullptr)
        {
            MmFreeContiguousMemory(m_virtualAddress);
            m_virtualAddress = nullptr;
            m_physicalAddress = 0;
        }
    }

public:
    ~ContiguousMemory() noexcept
    {
        this->Reset();
    }

    ContiguousMemory(const ContiguousMemory&) = delete;
    ContiguousMemory& operator=(const ContiguousMemory&) = delete;

    ContiguousMemory(ContiguousMemory&& other) noexcept
    {
        m_virtualAddress = other.m_virtualAddress;
        m_physicalAddress = other.m_physicalAddress;
        other.m_virtualAddress = nullptr;
        other.m_physicalAddress = 0;
    }

    ContiguousMemory& operator=(ContiguousMemory&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        this->Reset();
        m_virtualAddress = other.m_virtualAddress;
        m_physicalAddress = other.m_physicalAddress;
        other.m_virtualAddress = nullptr;
        other.m_physicalAddress = 0;

        return *this;
    }

    PVOID VirtualAddress()
    {
        return m_virtualAddress;
    }

    const PVOID VirtualAddress() const
    {
        return m_virtualAddress;
    }

    // we must return a reference here because we need to provide a pointer to this value when calling vmx instructions.
    // if we returned by value instead, we would've gotten an rvalue which we cannot take the address of.
    // note that this is a bit dangerous because the caller can modify this member and cause memory leaks
    // or cause double frees, but we must return a reference here so they just need to be careful.
    unsigned long long& PhysicalAddress()
    {
        return m_physicalAddress;
    }

    const unsigned long long& PhysicalAddress() const
    {
        return m_physicalAddress;
    }

    static Optional<ContiguousMemory> allocate(const UINT64 size)
    {
        PHYSICAL_ADDRESS maximumAddress;
        maximumAddress.QuadPart = MAXUINT64;
        const PVOID virtualAddress = MmAllocateContiguousMemory(size, maximumAddress);
        if (virtualAddress == nullptr)
        {
            return Optional<ContiguousMemory>();
        }

        RtlSecureZeroMemory(virtualAddress, size);

        const unsigned long long physicalAddress = MmGetPhysicalAddress(virtualAddress).QuadPart;
        return Optional<ContiguousMemory>(
            static_cast<ContiguousMemory&&>(ContiguousMemory(virtualAddress, physicalAddress)));
    }
};

class PoolBuffer
{
private:
    PVOID m_pointer;
    ULONG m_tag;

    PoolBuffer(PVOID pointer, ULONG tag) noexcept
        : m_pointer(pointer), m_tag(tag)
    {
    }

    void Reset() noexcept
    {
        if (m_pointer != nullptr)
        {
            ExFreePoolWithTag(m_pointer, m_tag);
            m_pointer = nullptr;
        }
    }

public:
    ~PoolBuffer() noexcept
    {
        this->Reset();
    }

    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    PoolBuffer(PoolBuffer&& other) noexcept
        : m_pointer(other.m_pointer), m_tag(other.m_tag)
    {
        other.m_pointer = nullptr;
    }

    PoolBuffer& operator=(PoolBuffer&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        this->Reset();
        m_pointer = other.m_pointer;
        m_tag = other.m_tag;
        other.m_pointer = nullptr;

        return *this;
    }

    PVOID Pointer()
    {
        return m_pointer;
    }

    const PVOID Pointer() const
    {
        return m_pointer;
    }

    static Optional<PoolBuffer> allocate(SIZE_T size, ULONG poolFlags, ULONG tag)
    {
        PVOID pool = ExAllocatePool2(poolFlags, size, tag);
        if (pool == nullptr)
        {
            return Optional<PoolBuffer>();
        }

        return Optional<PoolBuffer>(PoolBuffer(pool, tag));
    }
};

// the point of this class is to create a GDT for our own use so the cached host TR limit won't be
// affected, otherwise it would've disagreed with its TSS descriptor and could cause unexpected crashes
class HostGdt
{
private:
    PoolBuffer m_memory;
    UINT16 m_limit;

    HostGdt(PoolBuffer&& memory, UINT16 limit) noexcept
        : m_memory(static_cast<PoolBuffer&&>(memory)), m_limit(limit)
    {
    }

public:
    HostGdt(const HostGdt&) = delete;
    HostGdt& operator=(const HostGdt&) = delete;

    HostGdt(HostGdt&& other) noexcept
        : m_memory(static_cast<PoolBuffer&&>(other.m_memory)), m_limit(other.m_limit)
    {
    }

    HostGdt& operator=(HostGdt&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        m_memory = static_cast<PoolBuffer&&>(other.m_memory);
        m_limit = other.m_limit;

        return *this;
    }

    UINT64 Base() const noexcept
    {
        return reinterpret_cast<UINT64>(m_memory.Pointer());
    }

    // note that we don't call this function at all during VMCS initialization because the VMCS doesn't
    // contain a host GDTR limit field (intel forces it to be 0xFFFF on every vmexit no matter what).
    UINT16 Limit() const noexcept
    {
        return m_limit;
    }

    static Optional<HostGdt> allocate(
        const SYSTEM_DESCRIPTOR_TABLE_REGISTER& guestGdtr,
        UINT16 windowsTrSelector,
        UINT32 tssLimitForDescriptor)
    {
        const size_t guestSize = guestGdtr.Limit + 1;

        if (guestSize > PAGE_SIZE)
        {
            LOG_ERROR("HostGdt: guest GDT too large to fit in one page.");
            return Optional<HostGdt>();
        }

        // the TR selector's index part must fit within the guest GDT
        const UINT16 trOffset = windowsTrSelector & ~GDT_CONSTANTS::ALIGNMENT_MASK;
        if (trOffset + GDT_CONSTANTS::SYSTEM_DESCRIPTOR_SIZE > guestSize)
        {
            LOG_ERROR("HostGdt: TR selector offset 0x%X out of guest GDT range.", trOffset);
            return Optional<HostGdt>();
        }

        Optional<PoolBuffer> memory = PoolBuffer::allocate(
            PAGE_SIZE, POOL_FLAG_NON_PAGED, POOL_TAGS::HOST_GDT);
        if (!memory.has())
        {
            LOG_ERROR("HostGdt: failed to allocate GDT memory.");
            return Optional<HostGdt>();
        }

        // copying guest's GDT
        RtlCopyMemory(memory.value().Pointer(),
                      reinterpret_cast<void*>(guestGdtr.Base),
                      guestSize);

        // patching the TR descriptor's limit fields to match VMX's cached
        // host TR limit - only touches our own GDT copy, not the guest's GDT
        SYSTEM_SEGMENT_DESCRIPTOR_64* trDescriptor =
            reinterpret_cast<SYSTEM_SEGMENT_DESCRIPTOR_64*>(
                static_cast<UINT8*>(memory.value().Pointer()) + trOffset);

        trDescriptor->BaseDescriptor.Fields.LimitLow = tssLimitForDescriptor;
        trDescriptor->BaseDescriptor.Fields.LimitHigh = tssLimitForDescriptor >> BITS_16::HIGH_SHIFT;
        trDescriptor->BaseDescriptor.Fields.Granularity = 0;

        return Optional<HostGdt>(HostGdt(
            static_cast<PoolBuffer&&>(memory.value()),
            guestGdtr.Limit));
    }
};

// pins the current thread to a single core for the lifetime of this object.
// reverts to the previous affinity (i.e. cores code can run on) on destruction
class ScopedAffinity
{
public:
    explicit ScopedAffinity(const ULONG processorIndex) noexcept
    {
        m_oldAffinity = KeSetSystemAffinityThreadEx(1ull << processorIndex);
    }

    ~ScopedAffinity() noexcept
    {
        KeRevertToUserAffinityThreadEx(m_oldAffinity);
    }

    ScopedAffinity(const ScopedAffinity&) = delete;
    ScopedAffinity& operator=(const ScopedAffinity&) = delete;
    ScopedAffinity(ScopedAffinity&&) = delete;
    ScopedAffinity& operator=(ScopedAffinity&&) = delete;

private:
    KAFFINITY m_oldAffinity;
};

// owns the buffer returned by MmGetPhysicalMemoryRanges. the buffer is caller
// owned and must be released with ExFreePool.
class PhysicalMemoryRanges
{
private:
    explicit PhysicalMemoryRanges(PPHYSICAL_MEMORY_RANGE ranges) noexcept
        : m_ranges(ranges)
    {
    }

    PPHYSICAL_MEMORY_RANGE m_ranges;

    void Reset() noexcept
    {
        if (m_ranges != nullptr)
        {
            ExFreePool(m_ranges);
            m_ranges = nullptr;
        }
    }

public:
    ~PhysicalMemoryRanges() noexcept
    {
        Reset();
    }

    PhysicalMemoryRanges(const PhysicalMemoryRanges&) = delete;
    PhysicalMemoryRanges& operator=(const PhysicalMemoryRanges&) = delete;

    PhysicalMemoryRanges(PhysicalMemoryRanges&& other) noexcept
        : m_ranges(other.m_ranges)
    {
        other.m_ranges = nullptr;
    }

    PhysicalMemoryRanges& operator=(PhysicalMemoryRanges&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        Reset();
        m_ranges = other.m_ranges;
        other.m_ranges = nullptr;

        return *this;
    }

    // the array is terminated by an entry whose NumberOfBytes is zero
    PPHYSICAL_MEMORY_RANGE Ranges() const noexcept
    {
        return m_ranges;
    }

    static Optional<PhysicalMemoryRanges> query()
    {
        PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
        if (ranges == nullptr)
        {
            return Optional<PhysicalMemoryRanges>();
        }

        return Optional<PhysicalMemoryRanges>(PhysicalMemoryRanges(ranges));
    }
};
