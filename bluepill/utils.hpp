#pragma once

#if DBG
// in Debug mode, we print to the kernel debugger
#define LOG_ERROR(fmt, ...) DbgPrint("[-] ERROR: " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) DbgPrint("[+] " fmt "\n", ##__VA_ARGS__)
#else
// in Release mode, we strip the prints completely out of the compiled binary
#define LOG_ERROR(fmt, ...)
#define LOG_INFO(fmt, ...)
#endif

void __cdecl operator delete(void*, unsigned __int64);

template <class T>
class Optional
{
private:
    bool m_has;
    union
    {
        unsigned char m_dummy;
        T m_value;
    };

public:
    Optional() noexcept
        : m_has(false), m_dummy(0)
    {
    }

    Optional(const T& value) noexcept
        : m_has(true), m_value(value)
    {
    }

    Optional(T&& value)
        : m_has(true), m_value(static_cast<T&&>(value))
    {
    }

    Optional(const Optional& other)
        : m_has(other.m_has), m_value(other.m_value)
    {
    }

    Optional& operator=(const Optional& other)
    {
        if (this == &other)
        {
            return *this;
        }

        if (this->m_has)
        {
            (&this->m_value)->~T();
        }

        if (other.m_has)
        {
            this->m_value = other.m_value;
        }

        this->m_has = other.m_has;

        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this == &other)
        {
            return *this;
        }

        if (this->m_has)
        {
            (&this->m_value)->~T();
        }

        if (other.m_has)
        {
            this->m_value = static_cast<T&&>(other.m_value);
        }

        this->m_has = other.m_has;
        other.m_has = false;

        return *this;
    }

    ~Optional() noexcept
    {
        if (m_has)
        {
            (&m_value)->~T();
        }
    }

    bool has() const noexcept
    {
        return m_has;
    }

    T& value() noexcept
    {
        if (!m_has)
        {
            LOG_ERROR("Illegal Optional access.");
        }

        return m_value;
    }

    const T& value() const noexcept
    {
        return m_value;
    }

    void clear() noexcept
    {
        if (m_has)
        {
            (&m_value)->~T();
        }
        m_has = false;
    }
};

extern "C"
{
    USHORT AsmGetCs();
    USHORT AsmGetDs();
    USHORT AsmGetEs();
    USHORT AsmGetSs();
    USHORT AsmGetFs();
    USHORT AsmGetGs();
    USHORT AsmGetTr();
    USHORT AsmGetLdtr();
    void AsmGetGdtr(void* Gdtr);
    void AsmGetIdtr(void* Idtr);

    void AsmInveptAllContexts(INVEPT_DESCRIPTOR* descriptor);
    void AsmVmcall(UINT64 code, UINT64 arg1, UINT64 arg2, UINT64 arg3);
}
