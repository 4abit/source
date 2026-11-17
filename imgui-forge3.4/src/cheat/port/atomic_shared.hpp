#pragma once

// libc++ enforces trivially-copyable values in std::atomic and dropped the
// (deprecated in C++20) std::atomic<std::shared_ptr<T>> specializations that
// MSVC's standard library still ships. Vesta was written against MSVC-STL and
// uses this pattern in its publish/subscribe state; restore the exact same
// lock-based behavior here.

#include <memory>
#include <mutex>

namespace std {

template <typename T>
struct atomic<std::shared_ptr<T>>
{
	using value_type = std::shared_ptr<T>;

	static constexpr bool is_always_lock_free = false;

	atomic( ) = default;
	atomic( const atomic& ) = delete;
	atomic& operator=( const atomic& ) = delete;

	atomic( value_type desired ) noexcept
		: m_value( std::move( desired ) ) {}

	[[nodiscard]] bool is_lock_free( ) const noexcept { return false; }

	value_type load( memory_order = memory_order_seq_cst ) const noexcept
	{
		std::lock_guard lock( m_mutex );
		return m_value;
	}

	void store( value_type desired,
		memory_order = memory_order_seq_cst ) noexcept
	{
		std::lock_guard lock( m_mutex );
		m_value = std::move( desired );
	}

	operator value_type( ) const noexcept { return load( ); }

	value_type exchange( value_type desired,
		memory_order = memory_order_seq_cst ) noexcept
	{
		std::lock_guard lock( m_mutex );
		std::swap( m_value, desired );
		return desired;
	}

	bool compare_exchange_weak( value_type& expected, value_type desired,
		memory_order = memory_order_seq_cst,
		memory_order = memory_order_seq_cst ) noexcept
	{
		std::lock_guard lock( m_mutex );
		if ( m_value == expected )
		{
			m_value = std::move( desired );
			return true;
		}
		expected = m_value;
		return false;
	}

	bool compare_exchange_strong( value_type& expected, value_type desired,
		memory_order = memory_order_seq_cst,
		memory_order = memory_order_seq_cst ) noexcept
	{
		std::lock_guard lock( m_mutex );
		if ( m_value == expected )
		{
			m_value = std::move( desired );
			return true;
		}
		expected = m_value;
		return false;
	}

	value_type operator=( value_type desired ) noexcept
	{
		store( std::move( desired ) );
		return desired;
	}

  private:
	mutable std::mutex m_mutex{};
	value_type m_value{};
};

} // namespace std
