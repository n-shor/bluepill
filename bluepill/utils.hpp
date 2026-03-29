#pragma once

void __cdecl operator delete(void*, unsigned __int64);

template<class T> class Optional
{
private:
	bool m_has;
	union
	{
		unsigned char m_dummy;
		T m_value;
	};

public:
	Optional() noexcept : m_has(false), m_dummy(0)
	{
	}

	Optional(const T& value) noexcept : m_has(true), m_value(value)
	{
	}

	Optional(T&& value) : m_has(true), m_value(static_cast<T&&>(value))
	{
	}

	Optional(const Optional& other) : m_has(other.m_has), m_value(other.m_value)
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

	//TODO: handle access with has=false
	T& value() noexcept
	{
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
