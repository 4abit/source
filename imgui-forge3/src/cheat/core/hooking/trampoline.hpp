#pragma once

#include <cstddef>
#include <cstdint>

namespace hooking {

	// Inline detour with a relocated prologue, so the original stays callable
	// from any thread at any time (render workers included).
	class trampoline
	{
	public:
		bool install( void* target, void* detour );
		void remove();

		[[nodiscard]] bool active() const { return m_trampoline != nullptr; }

		template <typename fn_t>
		[[nodiscard]] fn_t original() const { return reinterpret_cast<fn_t>( m_trampoline ); }

	private:
		std::uint8_t* m_target{};
		void* m_trampoline{};
		std::uint8_t m_saved[ 32 ]{};
		std::size_t m_saved_length{};
	};

} // namespace hooking
