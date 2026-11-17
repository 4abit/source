#pragma once

#include <core/input/bindings.hpp>
#include <features/misc/input_lifecycle.hpp>

namespace features::misc {

	// Small features that live on the movement/combat tick and only need the input
	// gateway plus the world snapshot:
	//   * hit sound  - plays a short tone whenever a confirmed hit lands;
	//   * quick switch - swaps to the knife and back on one press;
	//   * afk guard  - taps a movement key so the server never marks the player idle;
	//   * knife bot  - attacks when an enemy is inside knife range and in front.
	class misc_extras_t
	{
	public:
		void tick( );

	private:
		using owned_control = input_lifecycle::owned_control;

		void tick_hit_sound( );
		void tick_quick_switch( );
		void tick_afk_guard( );
		void tick_knife_bot( );

		std::uint64_t m_last_hit_sequence{};
		std::chrono::steady_clock::time_point m_last_sound{};

		int m_quick_phase{}; // 0 idle, 1 knife, 2 back
		std::chrono::steady_clock::time_point m_quick_phase_started{};
		std::uint16_t m_quick_slot{};

		std::chrono::steady_clock::time_point m_last_afk_tap{};

		owned_control m_knife_attack{};
		std::chrono::steady_clock::time_point m_knife_attack_started{};

		// Grenade assist.
		void tick_grenade_assist( );
		owned_control m_grenade_attack{};
		owned_control m_grenade_jump{};
		std::chrono::steady_clock::time_point m_release_started{};
		std::chrono::steady_clock::time_point m_last_jump{};
		bool m_released{};
	};

	inline misc_extras_t& misc_extras( ) { static misc_extras_t value{}; return value; }

} // namespace features::misc
