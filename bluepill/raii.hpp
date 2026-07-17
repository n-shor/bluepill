#pragma once

#include "constants.hpp"
#include "utils.hpp"
#include <intrin.h>
#include <ntddk.h>

class ContiguousMemory
{
private:
    ContiguousMemory(PVOID virtualAddress, unsigned long long physicalAddress)
        : m_virtualAddress(virtualAddress), m_physicalAddress(physicalAddress)
    {
    }

    PVOID m_virtualAddress;
    unsigned long long m_physicalAddress;

public:
    ~ContiguousMemory() noexcept
    {
        if (m_virtualAddress != nullptr)
        {
            MmFreeContiguousMemory(m_virtualAddress);
            m_virtualAddress = nullptr;
            m_physicalAddress = 0;
        }
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

        this->~ContiguousMemory();
        m_virtualAddress = other.m_virtualAddress;
        m_physicalAddress = other.m_physicalAddress;
        other.m_virtualAddress = nullptr;
        other.m_physicalAddress = 0;

        return *this;
    }

    PVOID& VirtualAddress()
    {
        return m_virtualAddress;
    }

    const PVOID& VirtualAddress() const
    {
        return m_virtualAddress;
    }

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
    PoolBuffer(PVOID pointer, ULONG tag) noexcept
        : m_pointer(pointer), m_tag(tag)
    {
    }

    PVOID m_pointer;
    ULONG m_tag;

public:
    ~PoolBuffer() noexcept
    {
        if (m_pointer != nullptr)
        {
            ExFreePoolWithTag(m_pointer, m_tag);
            m_pointer = nullptr;
        }
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

        this->~PoolBuffer();
        m_pointer = other.m_pointer;
        m_tag = other.m_tag;
        other.m_pointer = nullptr;

        return *this;
    }

    PVOID& Pointer()
    {
        return m_pointer;
    }
    const PVOID& Pointer() const
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

// the point of this class is to create a GDT for our own use so the CPU's vmexit time
// busy bit write on host TR will happen on our GDT rather than on windows' GDT, which would trigger patchguard
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

    UINT16 Limit() const noexcept
    {
        return m_limit;
    }

    static Optional<HostGdt> allocate(
        const SYSTEM_DESCRIPTOR_TABLE_REGISTER& guestGdtr,
        UINT16 windowsTrSelector,
        UINT32 tssLimitForDescriptor)
    {
        const UINT16 guestSize = guestGdtr.Limit + 1;

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
            PAGE_SIZE, POOL_FLAG_NON_PAGED, HYPERVISOR_CONFIG::HOST_GDT_TAG);
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
