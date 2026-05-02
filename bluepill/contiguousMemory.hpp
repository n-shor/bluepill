#pragma once

#include "utils.hpp"
#include <ntddk.h>

class ContiguousMemory
{
private:
    ContiguousMemory(PVOID virtualAddress, unsigned long long physicalAddress)
        : m_virtualAddress(virtualAddress), m_physicalAddress(physicalAddress)
    {
        // LOG_INFO("ContiguousMemory() called.");
    }

    PVOID m_virtualAddress;
    unsigned long long m_physicalAddress;

public:
    ~ContiguousMemory() noexcept
    {
        // LOG_INFO("~ContiguousMemory(%d) called.", m_virtualAddress != nullptr);

        if (m_virtualAddress != nullptr)
        {
            MmFreeContiguousMemory(m_virtualAddress);
            m_virtualAddress = nullptr;
            m_physicalAddress = 0;
        }
    }

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

    static Optional<ContiguousMemory> allocate(const size_t size)
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
