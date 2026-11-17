#pragma once

#include <config/skins.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace features::skins {

	// Inventory changer for the local player: weapon finishes, knife and glove swaps
	// and the agent model. Runs on the game thread from frame_stage_notify, because the
	// regeneration calls (composite material, mesh groups, model swaps) are not
	// thread-safe.
	//
	// Only entities the local pawn owns are touched: the fallback fields exist on every
	// player's weapons, and writing those would change what everyone else sees.
	class changer_t
	{
	public:
		void on_frame_stage( int stage );

	private:
		// What was last written to an entity. The game resets item views when it
		// re-networks a weapon, so the live fields are compared against it every frame.
		struct applied_t
		{
			std::uint16_t def_index{};
			config::skin_finish finish{};
			// When the regeneration last ran. A field the game keeps resetting would
			// otherwise rebuild the composite material on every network update.
			std::uint64_t applied_ms{};
		};

		void weapons( std::uintptr_t pawn, std::uintptr_t controller, const config::skin_loadout& loadout );
		void gloves( std::uintptr_t pawn, std::uintptr_t controller, int team, const config::skin_loadout& loadout );
		void agent( std::uintptr_t pawn, const config::skin_loadout& loadout );
		void reset( std::uintptr_t pawn );

		std::uintptr_t m_pawn{};
		std::uint32_t m_active{};
		std::uintptr_t m_hud{};
		std::uint64_t m_hud_ms{};
		std::unordered_map<std::uint32_t, applied_t> m_weapons{};
		applied_t m_gloves{};
		bool m_gloves_applied{};
		std::string m_agent{};
		// The game's own model, captured before the first swap; picking the default agent
		// puts it back.
		std::string m_agent_original{};
		std::uintptr_t m_agent_handle{};
		// Same guard for the agent: set_model loads a model, it must not run every update.
		std::uint64_t m_agent_applied_ms{};
	};

	[[nodiscard]] inline changer_t& changer( )
	{
		static changer_t instance{};
		return instance;
	}

} // namespace features::skins
