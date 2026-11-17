#include <stdafx.hpp>

#include <features/skins/skins.hpp>

#include <app/context.hpp>
#include <core/state/services.hpp>
#include <render/chams/renderer.hpp>

#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace features::skins {
	namespace {

		// Item ids at or above 0xF0000000'00000000 are never handed out by the GC; the
		// client treats such an item as locally owned and paints it from the fallback fields.
		constexpr std::uint32_t k_item_id_high = 0xf0000000u;
		constexpr std::uint32_t k_item_id_low = 0x10u;
		constexpr std::size_t k_max_weapons = 64;

		// FSN stages the changer runs in: weapons right after the network data is in,
		// gloves and the agent once entities have run their post-data-update.
		constexpr int k_stage_net_update_postdataupdate_start = 6;
		constexpr int k_stage_net_update_postdataupdate_end = 7;

		// Glove finishes are attributes on the pawn's m_EconGloves item view.
		constexpr std::array<std::uint16_t, 3> k_glove_attribute_ids{ 6, 7, 8 };
		constexpr std::array<const char*, 3> k_glove_attribute_names{
			"set item texture prefab", "set item texture seed", "set item texture wear" };
		constexpr std::ptrdiff_t k_attribute_count = 0x210;
		constexpr std::ptrdiff_t k_attribute_data = 0x218;
		constexpr std::ptrdiff_t k_attribute_stride = 0x48;
		constexpr std::ptrdiff_t k_attribute_definition = 0x30;
		constexpr std::ptrdiff_t k_attribute_value = 0x34;

		// C_EconEntity's composite-material holder; not a schema field, so it is read
		// from the client's own `lea rcx, [rbx+owner]; mov dl, 1; call composite`.
		constexpr std::ptrdiff_t k_composite_material_owner_fallback = 0x608;
		constexpr std::size_t k_post_data_update = 10;

		using mesh_group_fn = void( __fastcall* )( std::uintptr_t, std::uint64_t );
		using composite_fn = void( __fastcall* )( std::uintptr_t, bool );
		using update_skin_fn = void( __fastcall* )( std::uintptr_t, bool );
		using update_subclass_fn = void( __fastcall* )( std::uintptr_t );
		using model_path_fn = const char*( __fastcall* )( std::uintptr_t );
		using set_model_fn = void( __fastcall* )( std::uintptr_t, const char* );
		using invalidate_fn = void( __fastcall* )( std::uintptr_t );
		using set_attribute_fn = void( __fastcall* )( std::uintptr_t, const char*, float );
		using bodygroup_fn = void( __fastcall* )( std::uintptr_t, int, unsigned );
		using post_data_update_fn = void( __fastcall* )( std::uintptr_t, int );
		using item_system_fn = std::uintptr_t( __fastcall* )( );

		// Weapon finishes are attributes too. C_EconEntity's PostDataUpdate copies the
		// fallback fields into them (paint kit, seed, wear, "kill eater") for an item the
		// inventory does not know; without that call the fallbacks are never read.
		constexpr std::array<const char*, 5> k_weapon_attribute_names{
			"set item texture prefab", "set item texture seed", "set item texture wear",
			"kill eater", "kill eater score type" };

		struct functions_t
		{
			post_data_update_fn econ_update{};
			mesh_group_fn mesh_group{};
			composite_fn composite{};
			update_skin_fn update_skin{};
			update_subclass_fn update_subclass{};
			model_path_fn model_path{};
			set_model_fn set_model{};
			invalidate_fn invalidate{};
			set_attribute_fn set_attribute{};
			bodygroup_fn bodygroup{};
			item_system_fn item_system{};
			std::ptrdiff_t composite_owner{ k_composite_material_owner_fallback };

			functions_t( )
			{
				auto& process = app::context().process;
				const auto client = process.module_base( "client.dll" );
				const auto scan = [ & ]( std::string_view signature ) -> std::uintptr_t {
					return client ? process.scan_signature( client, signature ) : 0;
				};

				// C_EconEntity::PostDataUpdate(this, type): moved one vtable slot in a game
				// update, so it is called directly instead of through the table.
				this->econ_update = reinterpret_cast<post_data_update_fn>(
					scan( "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ? ? ? ? F6 C3 01 74 ? 48 8B CF E8 ? ? ? ? C6 87" ) );
				this->mesh_group = reinterpret_cast<mesh_group_fn>(
					scan( "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71" ) );
				this->composite = reinterpret_cast<composite_fn>(
					scan( "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 44 0F B6 F2 48 8B F9" ) );
				this->update_skin = reinterpret_cast<update_skin_fn>(
					scan( "40 55 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 44 0F B6 FA 48 8B F1" ) );
				if ( !this->update_skin )
				{
					// The regenerate_weapon_skins command handler: composite( weapon + owner, true )
					// followed by the skin update on the same weapon.
					if ( const auto site = scan( "48 8D 8B ? ? 00 00 B2 01 E8 ? ? ? ? 33 D2 48 8B CB E8" ) )
						this->update_skin = reinterpret_cast<update_skin_fn>( process.decode_call( site + 19 ) );
				}
				this->update_subclass = reinterpret_cast<update_subclass_fn>(
					scan( "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 83 BB 88 03 00 00 00" ) );
				this->model_path = reinterpret_cast<model_path_fn>(
					scan( "48 89 5C 24 10 56 48 83 EC 20 48 8B 1D ? ? ? ?" ) );
				const auto set_model_site = scan( "48 8D 15 ? ? ? ? 48 8B CB E8 ? ? ? ? 48 8B D7 48 8B CB" );
				this->set_model = reinterpret_cast<set_model_fn>(
					set_model_site ? process.decode_call( set_model_site + 10 ) : 0 );
				this->invalidate = reinterpret_cast<invalidate_fn>(
					scan( "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 48 8D B9 ? ? ? ? 48 8B F1" ) );
				this->set_attribute = reinterpret_cast<set_attribute_fn>(
					scan( "40 53 48 83 EC 20 48 8B D9 48 81 C1 08 02 00 00" ) );
				this->bodygroup = reinterpret_cast<bodygroup_fn>(
					scan( "85 D2 0F 88 ? ? ? ? 55 56 57" ) );
				this->item_system = reinterpret_cast<item_system_fn>(
					scan( "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81" ) );
				if ( const auto owner_site = scan( "48 8D 8B ? ? 00 00 B2 01 E8" );
					owner_site && this->composite
					&& process.decode_call( owner_site + 9 ) == reinterpret_cast<std::uintptr_t>( this->composite ) )
				{
					const auto owner = process.load<std::int32_t>( owner_site + 3 );
					if ( owner > 0x400 && owner < 0x1000 )
						this->composite_owner = owner;
				}

				app::context().diagnostics.info(
					"[skins] owner={:#x} econ_update={} mesh={} composite={} skin={} subclass={} path={} model={} invalidate={} attr={} body={} items={}",
					this->composite_owner, this->econ_update != nullptr,
					this->mesh_group != nullptr, this->composite != nullptr, this->update_skin != nullptr,
					this->update_subclass != nullptr, this->model_path != nullptr, this->set_model != nullptr,
					this->invalidate != nullptr, this->set_attribute != nullptr, this->bodygroup != nullptr,
					this->item_system != nullptr );
			}

			[[nodiscard]] bool weapons_ready( ) const
			{
				return this->mesh_group && this->composite;
			}
		};

		[[nodiscard]] const functions_t& fn( )
		{
			static const functions_t value{};
			return value;
		}

		struct offsets_t
		{
			std::int32_t weapon_services = SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_id );
			std::int32_t my_weapons = SCHEMA( "CPlayer_WeaponServices", "m_hMyWeapons"_id );
			std::int32_t active_weapon = SCHEMA( "CPlayer_WeaponServices", "m_hActiveWeapon"_id );
			std::int32_t attribute_manager = SCHEMA( "C_EconEntity", "m_AttributeManager"_id );
			std::int32_t item = SCHEMA( "C_AttributeContainer", "m_Item"_id );
			std::int32_t def_index = SCHEMA( "C_EconItemView", "m_iItemDefinitionIndex"_id );
			std::int32_t quality = SCHEMA( "C_EconItemView", "m_iEntityQuality"_id );
			std::int32_t item_id = SCHEMA( "C_EconItemView", "m_iItemID"_id );
			std::int32_t item_id_high = SCHEMA( "C_EconItemView", "m_iItemIDHigh"_id );
			std::int32_t item_id_low = SCHEMA( "C_EconItemView", "m_iItemIDLow"_id );
			std::int32_t account_id = SCHEMA( "C_EconItemView", "m_iAccountID"_id );
			std::int32_t initialized = SCHEMA( "C_EconItemView", "m_bInitialized"_id );
			std::int32_t disallow_soc = SCHEMA( "C_EconItemView", "m_bDisallowSOC"_id );
			std::int32_t restore_material = SCHEMA( "C_EconItemView", "m_bRestoreCustomMaterialAfterPrecache"_id );
			std::int32_t custom_name = SCHEMA( "C_EconItemView", "m_szCustomName"_id );
			std::int32_t paint_kit = SCHEMA( "C_EconEntity", "m_nFallbackPaintKit"_id );
			std::int32_t seed = SCHEMA( "C_EconEntity", "m_nFallbackSeed"_id );
			std::int32_t wear = SCHEMA( "C_EconEntity", "m_flFallbackWear"_id );
			std::int32_t stat_trak = SCHEMA( "C_EconEntity", "m_nFallbackStatTrak"_id );
			std::int32_t subclass = SCHEMA( "C_BaseEntity", "m_nSubclassID"_id );
			std::int32_t scene_node = SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_id );
			std::int32_t team = SCHEMA( "C_BaseEntity", "m_iTeamNum"_id );
			std::int32_t hud_arms = SCHEMA( "C_CSPlayerPawn", "m_hHudModelArms"_id );
			std::int32_t gloves = SCHEMA( "C_CSPlayerPawn", "m_EconGloves"_id );
			std::int32_t reapply_gloves = SCHEMA( "C_CSPlayerPawn", "m_bNeedToReApplyGloves"_id );
			std::int32_t node_child = SCHEMA( "CGameSceneNode", "m_pChild"_id );
			std::int32_t node_sibling = SCHEMA( "CGameSceneNode", "m_pNextSibling"_id );
			std::int32_t node_owner = SCHEMA( "CGameSceneNode", "m_pOwner"_id );
			std::int32_t model_state = SCHEMA( "CSkeletonInstance", "m_modelState"_id );
			std::int32_t model_name = SCHEMA( "CModelState", "m_ModelName"_id );
			std::int32_t model_handle = SCHEMA( "CModelState", "m_hModel"_id );
			std::int32_t steam_id = SCHEMA( "CBasePlayerController", "m_steamID"_id );
			// StatTrak counts (and shows) only on a weapon whose original owner is the viewer.
			std::int32_t owner_xuid_low = SCHEMA( "C_EconEntity", "m_OriginalOwnerXuidLow"_id );
			std::int32_t owner_xuid_high = SCHEMA( "C_EconEntity", "m_OriginalOwnerXuidHigh"_id );

			[[nodiscard]] bool valid( ) const
			{
				return this->weapon_services && this->my_weapons && this->attribute_manager && this->item
					&& this->def_index && this->item_id_high && this->item_id_low && this->paint_kit
					&& this->subclass && this->scene_node;
			}
		};

		[[nodiscard]] const offsets_t& off( )
		{
			static const offsets_t value{};
			return value;
		}

		// The changer runs on the game thread (frame_stage_notify) over live entities, so
		// fields are touched directly; the process API is a ReadProcessMemory /
		// WriteProcessMemory kernel call per field, which the frame waited on.
		[[nodiscard]] bool addressable( const std::uintptr_t address )
		{
			return address >= 0x10000 && address < 0x00007FFFFFFFFFFFull;
		}

		template <class T>
		[[nodiscard]] T rd( const std::uintptr_t address )
		{
			T value{};
			if ( addressable( address ) )
				std::memcpy( &value, reinterpret_cast<const void*>( address ), sizeof( T ) );
			return value;
		}

		template <class T>
		void wr( const std::uintptr_t address, const T value )
		{
			if ( addressable( address ) )
				std::memcpy( reinterpret_cast<void*>( address ), &value, sizeof( T ) );
		}

		template <class Fn, class... Args>
		void call_vfunc( const std::uintptr_t object, const std::size_t index, Args... args )
		{
			const auto table = rd<std::uintptr_t>( object );
			const auto target = table ? rd<std::uintptr_t>( table + index * sizeof( void* ) ) : 0;
			if ( target )
				reinterpret_cast<Fn>( target )( object, args... );
		}

		[[nodiscard]] bool is_knife( const std::uint16_t def )
		{
			return def == 41 || def == 42 || def == 59 || def == 80 || ( def >= 500 && def <= 526 );
		}

		// m_nSubclassID of an econ entity is the lower-cased MurmurHash2 of its
		// definition index as text; the client resolves the model through it.
		[[nodiscard]] std::uint32_t subclass_token( const std::uint16_t def )
		{
			char text[ 8 ]{};
			const int length = std::snprintf( text, sizeof( text ), "%u", def );
			constexpr std::uint32_t m = 0x5bd1e995u;
			std::uint32_t h = 0x31415926u ^ static_cast<std::uint32_t>( length );
			int i = 0;
			int left = length;
			for ( ; left >= 4; i += 4, left -= 4 )
			{
				std::uint32_t k = static_cast<std::uint8_t>( text[ i ] )
					| static_cast<std::uint8_t>( text[ i + 1 ] ) << 8
					| static_cast<std::uint8_t>( text[ i + 2 ] ) << 16
					| static_cast<std::uint32_t>( static_cast<std::uint8_t>( text[ i + 3 ] ) ) << 24;
				k *= m;
				k ^= k >> 24;
				k *= m;
				h *= m;
				h ^= k;
			}
			switch ( left )
			{
			case 3: h ^= static_cast<std::uint32_t>( static_cast<std::uint8_t>( text[ i + 2 ] ) ) << 16; [[fallthrough]];
			case 2: h ^= static_cast<std::uint32_t>( static_cast<std::uint8_t>( text[ i + 1 ] ) ) << 8; [[fallthrough]];
			case 1: h ^= static_cast<std::uint8_t>( text[ i ] ); h *= m;
			}
			h ^= h >> 13;
			h *= m;
			h ^= h >> 15;
			return h;
		}

		// items_game.txt is the source of use_legacy_model. The schema flag at a fixed
		// offset misses kits whose record moved, and those finishes never show.
		[[nodiscard]] std::optional<bool> legacy_from_items( const std::uint16_t paint_kit )
		{
			static std::unordered_set<std::uint16_t> legacy{};
			static int state = 0;
			if ( state == 0 )
			{
				if ( !chams::g_renderer.vpk( ).is_open( ) )
					return std::nullopt;
				state = -1;
				if ( const auto* entry = chams::g_renderer.vpk( ).find( "scripts/items/items_game.txt" ) )
				{
					const auto bytes = chams::g_renderer.vpk( ).read( *entry );
					const std::string_view text{ reinterpret_cast<const char*>( bytes.data( ) ), bytes.size( ) };
					std::uint16_t current = 0;
					bool have = false;
					std::string_view previous{};
					for ( std::size_t at = text.find( '"' ); at != std::string_view::npos; )
					{
						const auto end = text.find( '"', at + 1 );
						if ( end == std::string_view::npos )
							break;
						const auto token = text.substr( at + 1, end - at - 1 );
						const auto previous_id = !previous.empty( ) && previous.size( ) < 6
							&& std::ranges::all_of( previous, []( unsigned char c ) { return c >= '0' && c <= '9'; } );
						if ( token == "name" && previous_id )
						{
							unsigned value = 0;
							for ( const char c : previous )
								value = value * 10u + static_cast<unsigned>( c - '0' );
							if ( value > 0 && value < 65536u )
							{
								current = static_cast<std::uint16_t>( value );
								have = true;
							}
						}
						else if ( have && previous == "use_legacy_model" && token == "1" )
							legacy.insert( current );
						previous = token;
						at = text.find( '"', end + 1 );
					}
					if ( !legacy.empty( ) )
						state = 1;
				}
			}
			if ( state != 1 )
				return std::nullopt;
			return legacy.contains( paint_kit );
		}

		// Legacy finishes are painted on the older UV layout, which lives in mesh group 2.
		// The flag is read from the item schema once it is loaded.
		[[nodiscard]] bool legacy_paint_kit( const std::uint16_t paint_kit )
		{
			if ( const auto from_file = legacy_from_items( paint_kit ) )
				return *from_file;

			static std::unordered_map<std::uint16_t, bool> table{};
			if ( table.empty( ) && fn( ).item_system )
			{
				const auto system = fn( ).item_system( );
				const auto schema = system ? rd<std::uintptr_t>( system + 0x8 ) : 0;
				const auto count = schema ? rd<int>( schema + 0x2f0 ) : 0;
				const auto nodes = schema ? rd<std::uintptr_t>( schema + 0x2f8 ) : 0;
				if ( nodes && count > 0 && count < 20000 )
				{
					for ( int index = 0; index < count; ++index )
					{
						const auto kit = rd<std::uintptr_t>( nodes + static_cast<std::uintptr_t>( index ) * 32 + 24 );
						if ( kit )
							table[ static_cast<std::uint16_t>( rd<int>( kit ) ) ] = rd<bool>( kit + 0xae );
					}
				}
			}
			const auto found = table.find( paint_kit );
			return found != table.end( ) && found->second;
		}

		[[nodiscard]] std::uintptr_t hud_weapon( const std::uintptr_t pawn )
		{
			const auto& o = off( );
			if ( !o.hud_arms || !o.node_child || !o.node_sibling || !o.node_owner )
				return 0;
			const auto arms = game::entity_index( ).lookup( rd<std::uint32_t>( pawn + o.hud_arms ) );
			const auto node = arms ? rd<std::uintptr_t>( arms + o.scene_node ) : 0;
			if ( !node )
				return 0;

			auto child = rd<std::uintptr_t>( node + o.node_child );
			for ( int guard = 0; child > 0x10000 && guard < 64; ++guard )
			{
				const auto owner = rd<std::uintptr_t>( child + o.node_owner );
				// Read the class off the entity. The directory list does not keep hud models,
				// so matching it there never found the first-person weapon.
				if ( owner > 0x10000 && game::entity_index( ).schema_of( owner ) == "C_CS2HudModelWeapon"_id )
					return owner;
				child = rd<std::uintptr_t>( child + o.node_sibling );
			}
			return 0;
		}

		void set_mesh_group( const std::uintptr_t entity, const bool legacy )
		{
			const auto node = entity ? rd<std::uintptr_t>( entity + off( ).scene_node ) : 0;
			if ( node && fn( ).mesh_group )
				fn( ).mesh_group( node, legacy ? 2 : 1 );
		}

		[[nodiscard]] bool same( const config::skin_finish& a, const config::skin_finish& b )
		{
			return a.paint_kit == b.paint_kit && a.wear == b.wear && a.seed == b.seed
				&& a.stat_trak == b.stat_trak && a.quality == b.quality
				&& std::strncmp( a.name, b.name, sizeof( a.name ) ) == 0;
		}

		[[nodiscard]] std::uint32_t account_of( const std::uintptr_t controller )
		{
			const auto& o = off( );
			return controller && o.steam_id
				? static_cast<std::uint32_t>( rd<std::uint64_t>( controller + o.steam_id ) & 0xffffffffu ) : 0;
		}

		void own_item( const std::uintptr_t view, const std::uint32_t account )
		{
			const auto& o = off( );
			wr<std::uint32_t>( view + o.item_id_high, k_item_id_high );
			wr<std::uint32_t>( view + o.item_id_low, k_item_id_low );
			if ( o.account_id )
				wr<std::uint32_t>( view + o.account_id, account );
			if ( o.initialized )
				wr<bool>( view + o.initialized, true );
		}

	} // namespace

	void changer_t::reset( const std::uintptr_t pawn )
	{
		this->m_pawn = pawn;
		this->m_active = 0;
		this->m_hud = 0;
		this->m_hud_ms = 0;
		this->m_weapons.clear( );
		this->m_gloves = {};
		this->m_gloves_applied = false;
		this->m_agent.clear( );
		this->m_agent_original.clear( );
		this->m_agent_handle = 0;
	}

	void changer_t::on_frame_stage( const int stage )
	{
		if ( stage != k_stage_net_update_postdataupdate_start && stage != k_stage_net_update_postdataupdate_end )
			return;

		const auto snapshot = config::get_runtime_snapshot( );
		if ( !snapshot || !snapshot->skin.enabled || !off( ).valid( ) )
			return;

		auto& local = game::local_player( );
		const auto pawn = local.pawn( );
		const auto controller = local.controller( );
		if ( !pawn || !controller || !local.alive( ) )
		{
			if ( this->m_pawn )
				this->reset( 0 );
			return;
		}
		if ( pawn != this->m_pawn )
			this->reset( pawn );

		const auto* loadout = snapshot->skin.for_team( rd<std::uint8_t>( pawn + off( ).team ) );
		if ( !loadout )
			return;

		if ( stage == k_stage_net_update_postdataupdate_start )
		{
			this->weapons( pawn, controller, *loadout );
			return;
		}
		this->gloves( pawn, controller, rd<std::uint8_t>( pawn + off( ).team ), *loadout );
		this->agent( pawn, *loadout );
	}

	void changer_t::weapons( const std::uintptr_t pawn, const std::uintptr_t controller,
		const config::skin_loadout& loadout )
	{
		const auto& o = off( );
		const auto& f = fn( );
		if ( !f.weapons_ready( ) )
			return;

		const auto services = rd<std::uintptr_t>( pawn + o.weapon_services );
		if ( !services )
			return;
		const auto count = rd<std::uint32_t>( services + o.my_weapons );
		const auto data = rd<std::uintptr_t>( services + o.my_weapons + 0x8 );
		if ( !data || !count || count > k_max_weapons )
			return;
		std::array<std::uint32_t, k_max_weapons> handles{};
		if ( !addressable( data ) )
			return;
		std::memcpy( handles.data( ), reinterpret_cast<const void*>( data ), count * sizeof( handles[ 0 ] ) );

		const auto active_handle = o.active_weapon ? rd<std::uint32_t>( services + o.active_weapon ) : 0;
		const auto active = game::entity_index( ).lookup( active_handle );
		const auto account = account_of( controller );
		const auto* knife = loadout.first( config::skin_kind::knife );

		for ( std::uint32_t index = 0; index < count; ++index )
		{
			const auto weapon = game::entity_index( ).lookup( handles[ index ] );
			if ( !weapon )
				continue;
			// The item view is not bound to its definition until the subclass data exists.
			if ( !rd<std::uintptr_t>( weapon + o.subclass + 0x8 ) )
				continue;

			const auto view = weapon + o.attribute_manager + o.item;
			const auto def = rd<std::uint16_t>( view + o.def_index );
			const bool knife_slot = is_knife( def );
			const config::skin_entry* entry = knife_slot ? knife : loadout.weapon( def );
			if ( !entry )
				continue;

			const auto target_def = knife_slot ? entry->def_index : def;
			const auto& finish = entry->finish;
			const auto found = this->m_weapons.find( handles[ index ] );
			const bool intact = rd<std::uint32_t>( view + o.item_id_high ) == k_item_id_high
				&& rd<std::int32_t>( weapon + o.paint_kit ) == static_cast<std::int32_t>( finish.paint_kit )
				&& def == target_def;
			if ( intact && found != this->m_weapons.end( ) && found->second.def_index == target_def
				&& same( found->second.finish, finish ) )
				continue;
			const auto now_ms = static_cast<std::uint64_t>( ::GetTickCount64( ) );

			if ( knife_slot && def != target_def )
				wr<std::uint16_t>( view + o.def_index, target_def );
			own_item( view, account );
			// Without these the client rebuilds the item from the real inventory and
			// drops finishes that are not already on the account.
			if ( o.restore_material )
				wr<bool>( view + o.restore_material, true );
			if ( o.disallow_soc )
				wr<bool>( view + o.disallow_soc, true );
			if ( o.quality && finish.quality >= 0 )
				wr<std::int32_t>( view + o.quality, finish.quality );
			wr<std::int32_t>( weapon + o.paint_kit, finish.paint_kit );
			if ( o.seed )
				wr<std::int32_t>( weapon + o.seed, finish.seed );
			if ( o.wear )
				wr<float>( weapon + o.wear, finish.wear );
			if ( o.stat_trak )
				wr<std::int32_t>( weapon + o.stat_trak, finish.stat_trak );
			if ( finish.stat_trak >= 0 && controller && o.steam_id && o.owner_xuid_low && o.owner_xuid_high )
			{
				const auto xuid = rd<std::uint64_t>( controller + o.steam_id );
				wr<std::uint32_t>( weapon + o.owner_xuid_low, static_cast<std::uint32_t>( xuid & 0xffffffffu ) );
				wr<std::uint32_t>( weapon + o.owner_xuid_high, static_cast<std::uint32_t>( xuid >> 32 ) );
			}
			if ( o.custom_name )
			{
				// The field is char[161]; the terminator is written with the text so a
				// shorter name never leaves the tail of the previous one behind.
				std::array<char, sizeof( finish.name )> name{};
				std::memcpy( name.data( ), finish.name, sizeof( finish.name ) - 1 );
				std::memcpy( reinterpret_cast<void*>( view + o.custom_name ), name.data( ), name.size( ) );
			}

			if ( knife_slot )
			{
				const auto token = subclass_token( target_def );
				if ( rd<std::uint32_t>( weapon + o.subclass ) != token )
				{
					wr<std::uint32_t>( weapon + o.subclass, token );
					if ( f.update_subclass )
						f.update_subclass( weapon );
					const char* path = f.model_path ? f.model_path( view ) : nullptr;
					if ( path && *path && f.set_model )
						f.set_model( weapon, path );
				}
			}

			const bool legacy = legacy_paint_kit( finish.paint_kit );
			set_mesh_group( weapon, legacy );
			if ( weapon == active )
				set_mesh_group( hud_weapon( pawn ), legacy );
			if ( f.econ_update )
				f.econ_update( weapon, 1 );
			// The client skips its fallback copy when the item id resolves to a real
			// inventory item, so the attributes are written here as well; re-setting an
			// existing attribute only overwrites its value.
			if ( f.set_attribute )
			{
				f.set_attribute( view, k_weapon_attribute_names[ 0 ], static_cast<float>( finish.paint_kit ) );
				f.set_attribute( view, k_weapon_attribute_names[ 1 ], static_cast<float>( finish.seed ) );
				f.set_attribute( view, k_weapon_attribute_names[ 2 ], finish.wear );
				if ( finish.stat_trak >= 0 )
				{
					// Unlike the paint fields, the kill count is stored bit-cast.
					f.set_attribute( view, k_weapon_attribute_names[ 3 ],
						std::bit_cast<float>( static_cast<std::uint32_t>( finish.stat_trak ) ) );
					f.set_attribute( view, k_weapon_attribute_names[ 4 ], 0.0f );
				}
			}
			f.composite( weapon + f.composite_owner, true );
			if ( f.update_skin )
				f.update_skin( weapon, true );
			if ( f.invalidate )
				f.invalidate( view );

			this->m_weapons[ handles[ index ] ] = { target_def, finish, now_ms };
		}

		// The first-person model is a separate entity. The game resets its mesh group
		// whenever it rebuilds the viewmodel, so the active weapon is pushed again
		// every update even when the world weapon's fields already match.
		const auto found = this->m_weapons.find( active_handle );
		if ( active && found != this->m_weapons.end( ) )
		{
			const bool legacy = legacy_paint_kit( found->second.finish.paint_kit );
			set_mesh_group( active, legacy );
			set_mesh_group( hud_weapon( pawn ), legacy );
		}
		this->m_active = active_handle;
	}

	void changer_t::gloves( const std::uintptr_t pawn, const std::uintptr_t controller, const int team,
		const config::skin_loadout& loadout )
	{
		const auto& o = off( );
		const auto& f = fn( );
		const auto* entry = loadout.first( config::skin_kind::gloves );
		if ( !entry || !o.gloves || !f.set_attribute )
			return;

		const auto view = pawn + o.gloves;
		const auto& finish = entry->finish;
		const auto paint_matches = [ & ]( ) {
			const auto count = rd<int>( view + k_attribute_count );
			const auto data = rd<std::uintptr_t>( view + k_attribute_data );
			if ( count <= 0 || count > 64 || !data )
				return false;
			const std::array<float, 3> wanted{
				static_cast<float>( finish.paint_kit ), static_cast<float>( finish.seed ), finish.wear };
			int matched = 0;
			for ( int index = 0; index < count; ++index )
			{
				const auto attribute = data + static_cast<std::uintptr_t>( index ) * k_attribute_stride;
				const auto id = rd<std::uint16_t>( attribute + k_attribute_definition );
				for ( std::size_t slot = 0; slot < k_glove_attribute_ids.size( ); ++slot )
				{
					if ( id == k_glove_attribute_ids[ slot ]
						&& rd<float>( attribute + k_attribute_value ) == wanted[ slot ] )
						++matched;
				}
			}
			return matched == 3;
		};

		const bool reapply = o.reapply_gloves && rd<bool>( pawn + o.reapply_gloves );
		if ( this->m_gloves_applied && !reapply && this->m_gloves.def_index == entry->def_index
			&& same( this->m_gloves.finish, finish )
			&& rd<std::uint16_t>( view + o.def_index ) == entry->def_index
			&& rd<std::uint32_t>( view + o.item_id_high ) == k_item_id_high && paint_matches( ) )
			return;
		// The flag below is ours until the game consumes it; seeing it again right after
		// writing it is not a reason to rebuild the gloves on every update.
		const auto now_ms = static_cast<std::uint64_t>( ::GetTickCount64( ) );
		if ( this->m_gloves_applied && this->m_gloves.def_index == entry->def_index
			&& same( this->m_gloves.finish, finish ) && now_ms - this->m_gloves.applied_ms < 1000 )
			return;

		wr<std::uint16_t>( view + o.def_index, entry->def_index );
		if ( o.item_id )
			wr<std::uint64_t>( view + o.item_id,
				static_cast<std::uint64_t>( k_item_id_high ) << 32 | k_item_id_low );
		own_item( view, account_of( controller ) );
		if ( o.restore_material )
			wr<bool>( view + o.restore_material, true );
		if ( o.disallow_soc )
			wr<bool>( view + o.disallow_soc, true );

		// The attributes are semantic floats: the paint kit is stored as 10068.0f, not
		// bit-cast, or the composite comes out invalid.
		f.set_attribute( view, k_glove_attribute_names[ 0 ], static_cast<float>( finish.paint_kit ) );
		f.set_attribute( view, k_glove_attribute_names[ 1 ], static_cast<float>( finish.seed ) );
		f.set_attribute( view, k_glove_attribute_names[ 2 ], finish.wear );
		if ( f.invalidate )
			f.invalidate( view );

		if ( o.reapply_gloves )
			wr<bool>( pawn + o.reapply_gloves, true );
		call_vfunc<void( __fastcall* )( std::uintptr_t, int )>( pawn, k_post_data_update, 1 );
		if ( f.bodygroup )
		{
			f.bodygroup( pawn, 0, 1u );
			f.bodygroup( pawn, team == 2 ? 0 : 1, 1u );
		}

		this->m_gloves = { entry->def_index, finish, now_ms };
		this->m_gloves_applied = true;
	}

	void changer_t::agent( const std::uintptr_t pawn, const config::skin_loadout& loadout )
	{
		const auto& o = off( );
		const auto& f = fn( );
		if ( !f.set_model || !o.model_state || !o.model_handle )
			return;
		const auto node = rd<std::uintptr_t>( pawn + o.scene_node );
		if ( !node )
			return;

		const auto state = node + o.model_state;
		const auto handle = rd<std::uintptr_t>( state + o.model_handle );

		if ( !loadout.agent_model[ 0 ] )
		{
			if ( !this->m_agent.empty( ) && !this->m_agent_original.empty( ) )
				f.set_model( pawn, this->m_agent_original.c_str( ) );
			this->m_agent.clear( );
			this->m_agent_original.clear( );
			this->m_agent_handle = 0;
			return;
		}

		// Respawns and team changes give the pawn a fresh game model; that one is what
		// the default agent means from then on. The engine spells paths its own way
		// (no "_c", other case or slashes) and swaps the handle a frame after set_model
		// while it loads, so our own agent must not be mistaken for the game's model.
		const auto same_model = []( std::string_view a, std::string_view b )
		{
			const auto trim = []( std::string_view s )
			{
				if ( s.ends_with( "_c" ) ) s.remove_suffix( 2 );
				return s;
			};
			a = trim( a );
			b = trim( b );
			if ( a.size( ) != b.size( ) )
				return false;
			for ( std::size_t i = 0; i < a.size( ); ++i )
			{
				auto x = static_cast< unsigned char >( a[ i ] == '\\' ? '/' : a[ i ] );
				auto y = static_cast< unsigned char >( b[ i ] == '\\' ? '/' : b[ i ] );
				if ( std::tolower( x ) != std::tolower( y ) )
					return false;
			}
			return true;
		};
		if ( this->m_agent.empty( ) || handle != this->m_agent_handle )
		{
			const auto text = o.model_name ? rd<std::uintptr_t>( state + o.model_name ) : 0;
			auto name = text ? app::context().process.load_text( text, 192 ) : std::string{};
			if ( !name.empty( ) && !same_model( name, this->m_agent ) && !same_model( name, loadout.agent_model ) )
				this->m_agent_original = std::move( name );
			// The handle moved only because our model finished loading: adopt it.
			else if ( !this->m_agent.empty( ) && same_model( name, this->m_agent ) )
				this->m_agent_handle = handle;
		}
		// Re-set only when the game swapped the model back (respawn, team change) or the
		// selection changed; comparing names would fight the engine's own path spelling.
		if ( this->m_agent == loadout.agent_model && handle == this->m_agent_handle )
			return;
		// The same agent again within a second: the model is still streaming in and
		// its handle has not settled; reloading it every update only costs frames.
		const auto now_ms = static_cast<std::uint64_t>( ::GetTickCount64( ) );
		if ( this->m_agent == loadout.agent_model && now_ms - this->m_agent_applied_ms < 1000 )
			return;

		f.set_model( pawn, loadout.agent_model );
		this->m_agent = loadout.agent_model;
		this->m_agent_handle = rd<std::uintptr_t>( state + o.model_handle );
		this->m_agent_applied_ms = now_ms;
	}

} // namespace features::skins
