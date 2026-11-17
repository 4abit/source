// Runtime probe: dumps the convar names and schema field offsets the features
// are about to depend on, so ambience and the skin changer are written against
// what this build actually exposes instead of guessed names.
//
// Armed by creating %TEMP%\vesta\probe.on; the probe consumes the marker, so
// re-creating it re-runs the dump without restarting the game.

#include <port/probe.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <app/context.hpp>
#include <core/memory/symbol.hpp>
#include <core/state/services.hpp>
#include <port/hook_hud.hpp>

namespace nl_cheat {
namespace {

	std::filesystem::path marker_path( )
	{
		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return {};
		return std::filesystem::path( temporary ) / L"vesta" / L"probe.on";
	}

	std::filesystem::path output_path( const wchar_t* name )
	{
		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return {};
		return std::filesystem::path( temporary ) / L"vesta" / name;
	}

	bool interesting_convar( const std::string& name )
	{
		static constexpr const char* k_filters[]{
			"fog", "sky", "ambient", "tonemap", "exposure", "light", "gamma",
			"mat_color", "r_draw", "cl_sky", "sun", "cloud", "prop", "fullbright",
			"mat_monitor", "mat_force", "sc_", "r_", "sv_", "cl_", "mp_",
		};
		for ( const auto* filter : k_filters )
		{
			if ( name.find( filter ) != std::string::npos )
				return true;
		}
		return false;
	}

	void dump_convars( std::ofstream& out )
	{
		const auto variables = game::variables( ).enumerate( );
		out << "convars=" << variables.size( ) << '\n';

		for ( const auto& variable : variables )
		{
			if ( !interesting_convar( variable.name ) )
				continue;

			float as_float{};
			int as_int{};
			const bool float_ok = game::variables( ).try_get( variable.pointer, as_float );
			const bool int_ok = game::variables( ).try_get( variable.pointer, as_int );

			out << variable.name;
			if ( float_ok )
				out << " float=" << as_float;
			if ( int_ok )
				out << " int=" << as_int;
			out << " ptr=0x" << std::hex << variable.pointer << std::dec << '\n';
		}
	}

	// Writes a test value into each candidate convar and reads it back a moment
	// later: the engine resets some of them immediately, and ambience must only
	// rely on the ones that actually hold.
	void dump_convar_write_test( std::ofstream& out )
	{
		static constexpr const char* k_int_candidates[]{
			"mat_fullbright", "r_drawskybox", "fog_override", "fog_enable",
			"fog_enableskybox", "sc_disable_baked_lighting",
			"lb_enable_stationary_lights", "lb_enable_dynamic_lights",
			"r_csgo_enable_volume_fog", "csgo_3d_skybox",
		};
		static constexpr const char* k_float_candidates[]{
			"r_fullscreen_gamma", "fog_override_start", "fog_override_end",
			"fog_override_max_density", "fog_override_exponent", "fog_start",
			"fog_end", "fog_maxdensity", "mat_tonemap_bloom_scale",
			"mat_tonemap_force_percent_target", "mat_tonemap_force_min",
		};

		for ( const auto* name : k_int_candidates )
		{
			const auto pointer = game::variables( ).find( identity::of( name ) );
			int original{};
			if ( !pointer || !game::variables( ).try_get( pointer, original ) )
			{
				out << name << " int: UNRESOLVED\n";
				continue;
			}
			const auto probe_value = original == 1 ? 0 : 1;
			const auto wrote = game::variables( ).set<int>( pointer, probe_value );
			std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
			const auto held = game::variables( ).get<int>( pointer );
			out << name << " int: wrote=" << wrote << " target=" << probe_value
				<< " read_back=" << held
				<< ( held == probe_value ? " HOLDS" : " REVERTED" ) << '\n';
			game::variables( ).set<int>( pointer, original );
		}

		for ( const auto* name : k_float_candidates )
		{
			const auto pointer = game::variables( ).find( identity::of( name ) );
			float original{};
			if ( !pointer || !game::variables( ).try_get( pointer, original ) )
			{
				out << name << " float: UNRESOLVED\n";
				continue;
			}
			const auto probe_value = original + 0.25f;
			const auto wrote = game::variables( ).set<float>( pointer, probe_value );
			std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
			const auto held = game::variables( ).get<float>( pointer );
			out << name << " float: wrote=" << wrote << " target=" << probe_value
				<< " read_back=" << held
				<< ( std::fabs( held - probe_value ) < 0.001f ? " HOLDS" : " REVERTED" ) << '\n';
			game::variables( ).set<float>( pointer, original );
		}
	}

	void dump_schema( std::ofstream& out )
	{
		struct entry
		{
			const char* class_name;
			const char* field;
		};

		static constexpr entry k_fields[]{
			// Weapon / econ item view.
			{ "C_EconEntity", "m_AttributeManager" },
			{ "C_AttributeContainer", "m_Item" },
			{ "C_EconItemView", "m_iItemDefinitionIndex" },
			{ "C_EconItemView", "m_iEntityQuality" },
			{ "C_EconItemView", "m_iItemIDHigh" },
			{ "C_EconItemView", "m_iItemIDLow" },
			{ "C_EconItemView", "m_iAccountID" },
			{ "C_EconItemView", "m_bInitialized" },
			{ "C_EconItemView", "m_nFallbackPaintKit" },
			{ "C_EconItemView", "m_nFallbackSeed" },
			{ "C_EconItemView", "m_nFallbackStatTrak" },
			{ "C_EconItemView", "m_flFallbackWear" },
			// CS2 renamed (or moved) the fallback block; the skinchanger needs to know
			// which of these the live schema actually carries.
			{ "C_EconItemView", "m_iFallbackPaintKit" },
			{ "C_EconItemView", "m_nPaintKit" },
			{ "C_EconItemView", "m_iPaintKit" },
			{ "C_EconItemView", "m_nCustomPaintKit" },
			{ "C_EconItemView", "m_fFallbackWear" },
			{ "C_EconItemView", "m_flWear" },
			{ "C_EconItemView", "m_fWear" },
			{ "C_EconItemView", "m_iFallbackSeed" },
			{ "C_EconItemView", "m_iFallbackStatTrak" },
			{ "C_EconItemView", "m_iStatTrak" },
			{ "C_EconItemView", "m_bInitializedPaintKit" },
			{ "C_EconItemView", "m_bDisallowSOC" },
			{ "C_EconItemView", "m_iEntityLevel" },
			{ "C_EconItemView", "m_iInventoryPosition" },
			{ "C_EconItemView", "m_NetworkedDynamicAttributes" },
			{ "C_EconItemView", "m_iTeam" },
			{ "C_EconItemView", "m_bRestoreCustomMaterialAfterPrecache" },
			{ "C_AttributeContainer", "m_Item" },
			{ "C_AttributeContainer", "m_iReapplyProvisionParity" },
			{ "C_AttributeContainer", "m_Outer" },
			{ "C_EconEntity", "m_AttributeManager" },
			{ "C_EconEntity", "m_bAttachmentDirty" },
			{ "C_EconEntity", "m_nFallbackPaintKit" },
			{ "C_EconEntity", "m_nFallbackSeed" },
			{ "C_EconEntity", "m_nFallbackStatTrak" },
			{ "C_EconEntity", "m_flFallbackWear" },
			{ "C_EconEntity", "m_iOldTeam" },
			{ "C_EconEntity", "m_bDynamicAccumulateables" },
			{ "C_BasePlayerWeapon", "m_iItemDefinitionIndex" },
			{ "C_BasePlayerWeapon", "m_nFallbackPaintKit" },
			{ "CPlayer_WeaponServices", "m_hMyWeapons" },
			{ "CPlayer_WeaponServices", "m_hActiveWeapon" },
			{ "C_BasePlayerPawn", "m_hMyWearables" },
			{ "C_BasePlayerPawn", "m_pItemServices" },
			{ "C_BaseEntity", "m_iHealth" },
			{ "C_BaseEntity", "m_iTeamNum" },
			{ "C_BaseEntity", "m_nModelIndex" },
			{ "C_CSPlayerPawn", "m_nLastSpawnedTime" },
			{ "C_CSPlayerPawnBase", "m_flFlashOverlayAlpha" },
			// Environment / ambience entities.
			{ "C_EnvSky", "m_hSkyMaterial" },
			{ "C_EnvSky", "m_vTintColor" },
			{ "C_EnvSky", "m_flBrightnessScale" },
			{ "C_EnvFogController", "m_fogColor" },
			{ "C_EnvFogController", "m_fogColorPrimary" },
			{ "C_EnvFogController", "m_fogColorSecondary" },
			{ "C_EnvFogController", "m_fogStart" },
			{ "C_EnvFogController", "m_fogEnd" },
			{ "C_EnvFogController", "m_fogMaxDensity" },
			{ "C_EnvFogController", "m_fogMinDensity" },
			{ "C_EnvFogController", "m_fogFalloff" },
			{ "C_EnvFogController", "m_bFogEnable" },
			{ "C_EnvFogController", "m_fogEnable" },
			{ "C_EnvFogController", "m_bFogOverride" },
			{ "C_EnvFogController", "m_flFogMaxOpacity" },
			{ "C_EnvFogController", "m_flFogMaxDensity" },
			{ "C_EnvVolumetricFogController", "m_flScattering" },
			{ "C_EnvVolumetricFogController", "m_flDensity" },
			{ "C_EnvVolumetricFogController", "m_vTintColor" },
			{ "C_EnvTonemapController", "m_bUseCustomAutoExposureMin" },
			{ "C_EnvTonemapController", "m_bUseCustomAutoExposureMax" },
			{ "C_EnvTonemapController", "m_flCustomAutoExposureMin" },
			{ "C_EnvTonemapController", "m_flCustomAutoExposureMax" },
			{ "C_EnvTonemapController", "m_flCustomBloomScale" },
			{ "C_EnvTonemapController", "m_flCustomBloomScaleMinimum" },
			{ "C_EnvTonemapController", "m_bUseCustomBloomScale" },
			{ "C_EnvTonemapController", "m_flBloomExponent" },
			{ "C_EnvTonemapController", "m_flBloomSaturation" },
			{ "C_EnvTonemapController", "m_bUseCustomVignette" },
			{ "C_EnvTonemapController", "m_flCustomVignetteStrength" },
			{ "C_EnvTonemapController", "m_flCustomContrast" },
			{ "C_EnvTonemapController", "m_flCustomBrightness" },
		};

		for ( const auto& field : k_fields )
		{
			const auto offset = game::fields( ).lookup(
				field.class_name, identity::of( field.field ) );
			out << field.class_name << "::" << field.field << " = ";
			if ( offset > 0 )
				out << "0x" << std::hex << offset << std::dec;
			else
				out << "NOT FOUND";
			out << '\n';
		}

		out << "total_fields=" << game::fields( ).field_count( ) << '\n';
	}

} // namespace

	void run_probe_if_armed( )
	{
		const auto marker = marker_path( );
		if ( marker.empty( ) )
			return;

		std::error_code error{};
		if ( !std::filesystem::exists( marker, error ) )
			return;

		// Wait until the game actually presents: sampling earlier reports vanilla
		// state and hides what the features did.
		if ( hook_hud::frames_rendered( ) < 30 )
			return;

		std::filesystem::remove( marker, error );

		{
			std::ofstream marker_log( output_path( L"probe_ran.log" ), std::ios::app );
			if ( marker_log )
				marker_log << "probe ran\n";
		}

		{
			std::ofstream out( output_path( L"probe_convars.txt" ), std::ios::trunc );
			if ( out )
				dump_convars( out );
		}
		{
			std::ofstream out( output_path( L"probe_schema.txt" ), std::ios::trunc );
			if ( out )
				dump_schema( out );
		}
		{
			std::ofstream out( output_path( L"probe_write_test.txt" ), std::ios::trunc );
			if ( out )
				dump_convar_write_test( out );
		}
	}

} // namespace nl_cheat
