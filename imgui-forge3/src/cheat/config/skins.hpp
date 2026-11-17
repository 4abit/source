#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace config {

	enum class skin_kind : std::uint8_t
	{
		weapon,
		knife,
		gloves,
	};

	// What gets painted on an item. Kept apart from the item it is put on so a finish
	// is never tied to the weapon it was designed for: any paint kit can go on any item,
	// and per-finish overrides (custom palettes and the like) belong here.
	struct skin_finish
	{
		std::uint16_t paint_kit{};
		float wear{ 0.01f };
		int seed{};
		int stat_trak{ -1 }; // kill count, -1 = no StatTrak
		int quality{ -1 };   // m_iEntityQuality, -1 = leave the item's own
		char name[ 32 ]{};   // name tag, empty = none
		// Chams drawn over the item in game: a visual_profile::chams material type, -1 = none.
		int chams_type{ -1 };
		std::array<std::uint8_t, 4> chams_color{ 255, 255, 255, 255 };
		float chams_exponent{ 50.0f }; // 0..100, the menu's slider
	};

	struct skin_entry
	{
		skin_kind kind{ skin_kind::weapon };
		// weapon: the definition the entry applies to; knife / gloves: the definition the
		// player's knife or gloves are turned into.
		std::uint16_t def_index{};
		skin_finish finish{};
	};

	struct skin_loadout
	{
		static constexpr std::size_t capacity{ 48 };

		std::size_t count{};
		std::array<skin_entry, capacity> entries{};
		char agent_model[ 160 ]{};

		[[nodiscard]] const skin_entry* weapon( const std::uint16_t def_index ) const
		{
			for ( std::size_t index = 0; index < count; ++index )
			{
				if ( entries[ index ].kind == skin_kind::weapon && entries[ index ].def_index == def_index )
					return &entries[ index ];
			}
			return nullptr;
		}

		[[nodiscard]] const skin_entry* first( const skin_kind kind ) const
		{
			for ( std::size_t index = 0; index < count; ++index )
			{
				if ( entries[ index ].kind == kind )
					return &entries[ index ];
			}
			return nullptr;
		}
	};

	// Plain data: copied into the runtime snapshot that the game thread reads.
	struct skin_profile
	{
		bool enabled{};
		std::array<skin_loadout, 2> teams{}; // [0] = CT, [1] = T

		[[nodiscard]] const skin_loadout* for_team( const int team_num ) const
		{
			if ( team_num == 3 ) return &teams[ 0 ];
			if ( team_num == 2 ) return &teams[ 1 ];
			return nullptr;
		}
	};

} // namespace config
