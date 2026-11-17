#include <stdafx.hpp>

#include <features/misc/ambience.hpp>
#include <features/misc/weather.hpp>
#include <features/world/world_hooks.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <app/context.hpp>
#include <config/settings.hpp>
#include <core/memory/symbol.hpp>
#include <core/state/services.hpp>

namespace features::misc {
namespace {

	namespace hash {
		constexpr std::uint32_t gradient_fog = 0x4B01FF63;
		constexpr std::uint32_t gradient_fog_2 = 0x0AA49C2A;
		constexpr std::uint32_t gradient_fog_3 = 0xFBF6448D;
		constexpr std::uint32_t enable_gradient_fog = 0x6E0FAD7E;
		constexpr std::uint32_t dof = 0x2ACAB07C;
		constexpr std::uint32_t bloom_scale = 0x565EAF76;
		constexpr std::uint32_t bloom_threshold = 0xBA98A9B0;
		constexpr std::uint32_t bloom_width = 0x2AE72B37;
		constexpr std::uint32_t bloom_strength = 0xB692902E;
		constexpr std::uint32_t bloom_skybox = 0x1313A424;
		constexpr std::uint32_t gamma = 0x24470A87;
	}

	// Convars behind each removal, first one the build knows wins.
	constexpr const char* k_removal_vars[ 6 ][ 3 ] = {
		{ nullptr, nullptr, nullptr },
		{ nullptr, nullptr, nullptr },
		{ "mat_postprocess_enable", "r_postprocess_enable", nullptr },
		{ "lb_enable_shadow_casting", "cl_csm_enabled", "r_shadows" },
		{ "r_drawskybox", "r_draw3dskybox", nullptr },
		{ "r_drawparticles", nullptr, nullptr },
	};

	struct alignas( 16 ) vec4
	{
		float x, y, z, w;
	};

	[[nodiscard]] const auto& settings()
	{
		return config::visual_settings.m_ambience;
	}

	[[nodiscard]] float unit( std::uint8_t value )
	{
		return static_cast<float>( value ) / 255.0f;
	}

	[[nodiscard]] std::uint8_t mix_channel( std::uint8_t night, float amount )
	{
		return static_cast<std::uint8_t>( std::lround( 255.0f + ( static_cast<float>( night ) - 255.0f ) * amount ) );
	}

	[[nodiscard]] bool plausible( std::uintptr_t address )
	{
		return address >= 0x10000 && address < 0x00007FFFFFFFFFFFull;
	}

	[[nodiscard]] float flash()
	{
		return weather().flash_level();
	}

} // namespace

void ambience_t::apply( )
{
	const auto& cfg = settings( );
	for ( int index = 0; index < 6; ++index )
	{
		auto& state = m_removals[ index ];
		const auto want = cfg.enabled && cfg.removals[ index ];
		if ( want == state.active )
			continue;
		for ( const auto* name : k_removal_vars[ index ] )
		{
			if ( !name )
				continue;
			const auto pointer = game::variables( ).find( identity::of( name ) );
			if ( !pointer )
				continue;
			if ( want )
			{
				int current = 1;
				( void )game::variables( ).try_get<int>( pointer, current );
				state.saved = current;
				( void )game::variables( ).set<int>( pointer, 0 );
			}
			else
				( void )game::variables( ).set<int>( pointer, state.saved );
		}
		state.active = want;
	}
	// Night mode colors the world in the scene hooks. Exposure would darken the
	// players along with it, so a value written earlier is only restored here.
	this->apply_exposure( );
	this->apply_tonemap( false, 1.0f );
}

void ambience_t::apply_exposure( )
{
	struct fields
	{
		std::int32_t min = SCHEMA( "C_PostProcessingVolume", "m_flMinExposure"_id );
		std::int32_t max = SCHEMA( "C_PostProcessingVolume", "m_flMaxExposure"_id );
		std::int32_t compensation = SCHEMA( "C_PostProcessingVolume", "m_flExposureCompensation"_id );
		std::int32_t control = SCHEMA( "C_PostProcessingVolume", "m_bExposureControl"_id );
		[[nodiscard]] bool valid( ) const { return min > 0 && max > 0 && control > 0; }
	};
	static const fields f{};
	if ( !f.valid( ) )
		return;

	auto& process = app::context().process;
	const auto& cfg = settings( );
	// Left false on purpose: see apply(). The restore path still runs.
	const auto night = false;
	static_cast<void>( cfg );
	// Exposure is a multiplier on the whole frame: full night keeps an eighth of the light.
	const auto amount = std::clamp( cfg.nightmode_exposure, 0.0f, 1.0f );
	const auto scale = night ? 1.0f - amount * 0.875f : 1.0f;

	const auto entities = game::entity_index( ).all( );
	if ( !entities )
		return;

	std::unordered_map<std::uintptr_t, exposure_state> seen{};
	for ( const auto& entry : *entities )
	{
		if ( entry.schema_id != "C_PostProcessingVolume"_id || !plausible( entry.ptr ) )
			continue;
		const auto volume = entry.ptr;
		auto saved = this->m_exposure.find( volume );
		if ( !night )
		{
			if ( saved != this->m_exposure.end( ) )
			{
				const auto& s = saved->second;
				( void )process.store<float>( volume + f.min, s.min );
				( void )process.store<float>( volume + f.max, s.max );
				if ( f.compensation > 0 )
					( void )process.store<float>( volume + f.compensation, s.compensation );
				( void )process.store<bool>( volume + f.control, s.control );
			}
			continue;
		}

		exposure_state original{};
		if ( saved != this->m_exposure.end( ) )
			original = saved->second;
		else
		{
			original.min = process.load<float>( volume + f.min );
			original.max = process.load<float>( volume + f.max );
			original.compensation = f.compensation > 0 ? process.load<float>( volume + f.compensation ) : 0.0f;
			original.control = process.load<bool>( volume + f.control );
		}
		// A volume without exposure control renders at a fixed exposure of one, so the
		// night value is fixed there too; one with control keeps adapting, scaled down.
		const auto low = original.control && original.min > 0.0f ? original.min : 1.0f;
		const auto high = original.control && original.max >= low ? original.max : low;
		( void )process.store<float>( volume + f.min, low * scale );
		( void )process.store<float>( volume + f.max, high * scale );
		( void )process.store<bool>( volume + f.control, true );
		seen.emplace( volume, original );
	}
	// Volumes that left the entity list (map change) are forgotten; a volume that came
	// back with new memory is read fresh next time.
	this->m_exposure = night ? std::move( seen ) : decltype( this->m_exposure ){};
	this->m_exposure_scale = scale;
}

// Maps without a post-processing volume take their auto exposure range from the
// env_tonemap_controller2 the camera points at; its range is scaled the same way.
void ambience_t::apply_tonemap( bool night, float scale )
{
	static const auto min_at = SCHEMA( "C_TonemapController2", "m_flAutoExposureMin"_id );
	static const auto max_at = SCHEMA( "C_TonemapController2", "m_flAutoExposureMax"_id );
	if ( min_at <= 0 || max_at <= 0 )
		return;
	const auto entities = game::entity_index( ).all( );
	if ( !entities )
		return;

	auto& process = app::context().process;
	std::unordered_map<std::uintptr_t, tonemap_state> seen{};
	for ( const auto& entry : *entities )
	{
		if ( entry.schema_id != "C_TonemapController2"_id || !plausible( entry.ptr ) )
			continue;
		const auto controller = entry.ptr;
		const auto saved = this->m_tonemap.find( controller );
		if ( !night )
		{
			if ( saved != this->m_tonemap.end( ) )
			{
				( void )process.store<float>( controller + min_at, saved->second.min );
				( void )process.store<float>( controller + max_at, saved->second.max );
			}
			continue;
		}

		tonemap_state original{};
		if ( saved != this->m_tonemap.end( ) )
			original = saved->second;
		else
		{
			original.min = process.load<float>( controller + min_at );
			original.max = process.load<float>( controller + max_at );
		}
		const auto low = original.min > 0.0f ? original.min : 1.0f;
		const auto high = original.max >= low ? original.max : low;
		( void )process.store<float>( controller + min_at, low * scale );
		( void )process.store<float>( controller + max_at, high * scale );
		seen.emplace( controller, original );
	}
	this->m_tonemap = night ? std::move( seen ) : decltype( this->m_tonemap ){};
}

ambience_t::skybox_token ambience_t::on_skybox_pre( std::uintptr_t meshes, int count ) const
{
	skybox_token token{};
	const auto& cfg = settings( );
	const auto boost = flash( );
	const auto tint = cfg.enabled && cfg.sky_tint_override;
	if ( ( !tint && boost <= 0.0f ) || !plausible( meshes ) || count <= 0 || count > 4096 )
		return token;

	// Scenesystem sky records are 0x70 bytes; the last one keeps its
	// descriptor 0x58 bytes before the end of the array.
	const auto descriptor = *reinterpret_cast<const std::uintptr_t*>(
		meshes + static_cast<std::uintptr_t>( count ) * 0x70 - 0x58 );
	if ( !plausible( descriptor ) )
		return token;
	auto* color = reinterpret_cast<float*>( descriptor + 0xE8 );
	std::memcpy( token.color, color, sizeof( token.color ) );
	token.descriptor = descriptor;

	float out[ 3 ]{ token.color[ 0 ], token.color[ 1 ], token.color[ 2 ] };
	if ( tint )
	{
		const auto brightness = std::clamp( cfg.sky_brightness, 0.0f, 10.0f );
		out[ 0 ] = unit( cfg.sky_tint.r ) * brightness;
		out[ 1 ] = unit( cfg.sky_tint.g ) * brightness;
		out[ 2 ] = unit( cfg.sky_tint.b ) * brightness;
	}
	if ( boost > 0.0f )
	{
		const auto& bolt = config::visual_settings.m_weather.lightning_color;
		out[ 0 ] += unit( bolt.r ) * boost * 4.0f;
		out[ 1 ] += unit( bolt.g ) * boost * 4.0f;
		out[ 2 ] += unit( bolt.b ) * boost * 4.0f;
	}
	std::memcpy( color, out, sizeof( out ) );
	return token;
}

void ambience_t::on_skybox_post( const skybox_token& token ) const
{
	if ( token.descriptor )
		std::memcpy( reinterpret_cast<float*>( token.descriptor + 0xE8 ), token.color, sizeof( token.color ) );
}

bool ambience_t::on_light_pre( std::uintptr_t object, float saved[ 3 ] ) const
{
	const auto& cfg = settings( );
	const auto boost = flash( );
	const auto flat = cfg.enabled && cfg.removals[ 1 ];
	const auto bright = cfg.enabled && cfg.fullbright;
	const auto sun = cfg.enabled && cfg.sunlight;
	const auto night = cfg.enabled && cfg.nightmode && !bright;
	if ( !plausible( object ) || ( !flat && !bright && !sun && !night && boost <= 0.0f ) )
		return false;

	auto* color = reinterpret_cast<float*>( object + 0xE4 );
	std::memcpy( saved, color, sizeof( float ) * 3 );
	float out[ 3 ]{ saved[ 0 ], saved[ 1 ], saved[ 2 ] };
	if ( bright )
	{
		const auto intensity = std::clamp( cfg.fullbright_intensity, 1.0f, 8.0f );
		out[ 0 ] = unit( cfg.fullbright_color.r ) * intensity;
		out[ 1 ] = unit( cfg.fullbright_color.g ) * intensity;
		out[ 2 ] = unit( cfg.fullbright_color.b ) * intensity;
	}
	else if ( sun )
	{
		const auto intensity = std::clamp( cfg.sunlight_intensity, 0.0f, 8.0f );
		out[ 0 ] = unit( cfg.sunlight_color.r ) * intensity;
		out[ 1 ] = unit( cfg.sunlight_color.g ) * intensity;
		out[ 2 ] = unit( cfg.sunlight_color.b ) * intensity;
	}
	else if ( flat )
		out[ 0 ] = out[ 1 ] = out[ 2 ] = 1.0f;

	// Night mode on the world: the sun and the other scene lights are what light
	// walls and floors in real time (aggregate geometry ignores the per-object
	// tint), so they are dimmed and shifted towards the night colour.
	if ( night )
	{
		const auto amount = std::clamp( cfg.nightmode_exposure, 0.0f, 1.0f );
		const auto dim = 1.0f - amount * 0.85f;
		out[ 0 ] *= unit( mix_channel( cfg.nightmode_color.r, amount ) ) * dim;
		out[ 1 ] *= unit( mix_channel( cfg.nightmode_color.g, amount ) ) * dim;
		out[ 2 ] *= unit( mix_channel( cfg.nightmode_color.b, amount ) ) * dim;
	}

	if ( boost > 0.0f )
	{
		const auto& bolt = config::visual_settings.m_weather.lightning_color;
		out[ 0 ] += unit( bolt.r ) * boost * 6.0f;
		out[ 1 ] += unit( bolt.g ) * boost * 6.0f;
		out[ 2 ] += unit( bolt.b ) * boost * 6.0f;
	}
	std::memcpy( color, out, sizeof( out ) );
	return true;
}

void ambience_t::on_light_post( std::uintptr_t object, const float saved[ 3 ] ) const
{
	std::memcpy( reinterpret_cast<float*>( object + 0xE4 ), saved, sizeof( float ) * 3 );
}

	[[nodiscard]] std::uintptr_t light_queue( )
	{
		static std::uintptr_t global{};
		static bool tried{};
		if ( tried )
			return global;
		tried = true;
		const auto scenes = app::context().process.module_base( "scenesystem.dll" );
		const auto site = scenes ? app::context().process.scan_signature( scenes,
			"48 8B 05 ? ? ? ? 48 C1 E1 04" ) : 0;
		global = site ? app::context().process.decode_rip( site, 3, 7 ) : 0;
		return global;
	}

void ambience_t::on_scene_objects( std::uintptr_t, int ) const
{
	// These batches are the sky and the small props. Walls are aggregate light
	// records, tinted in on_aggregate. Coloring this path painted the wrong things.
}

void ambience_t::on_aggregate( std::uintptr_t object_array ) const
{
	const auto& cfg = settings( );
	if ( !cfg.enabled || !cfg.nightmode || !plausible( object_array ) )
		return;
	const auto amount = std::clamp( cfg.nightmode_exposure, 0.0f, 1.0f );
	if ( amount <= 0.001f )
		return;

	const auto object_data = *reinterpret_cast<const std::uintptr_t*>( object_array + 0x8 );
	const auto queue_global = light_queue( );
	const auto queue = queue_global ? *reinterpret_cast<const std::uintptr_t*>( queue_global ) : 0;
	const auto base = queue ? *reinterpret_cast<const std::uintptr_t*>( queue + 0x18 ) : 0;
	if ( !plausible( object_data ) || !plausible( base ) )
		return;

	const auto count = *reinterpret_cast<const int*>( object_data + 0x4 );
	const auto index = *reinterpret_cast<const int*>( object_data + 0x30 );
	if ( count <= 0 || count > ( 1 << 20 ) || index < 0 || index > ( 1 << 22 ) )
		return;

	const float r = static_cast<float>( mix_channel( cfg.nightmode_color.r, amount ) ) / 255.0f;
	const float g = static_cast<float>( mix_channel( cfg.nightmode_color.g, amount ) ) / 255.0f;
	const float b = static_cast<float>( mix_channel( cfg.nightmode_color.b, amount ) ) / 255.0f;
	// Each light record is 48 bytes. The first three floats are the wall color;
	// a 32-byte byte tint landed on the sky and the small meshes instead.
	constexpr std::size_t stride = 48;
	for ( int i = 0; i < count; ++i )
	{
		auto* sample = reinterpret_cast<float*>(
			base + ( static_cast<std::size_t>( index ) + static_cast<std::size_t>( i ) ) * stride );
		const auto plausible_light = []( float value )
		{
			return std::isfinite( value ) && value >= 0.0f && value <= 8.0f;
		};
		if ( !plausible_light( sample[ 0 ] ) || !plausible_light( sample[ 1 ] ) || !plausible_light( sample[ 2 ] ) )
			continue;
		sample[ 0 ] = r;
		sample[ 1 ] = g;
		sample[ 2 ] = b;
	}
}

bool ambience_t::on_fog( void* output, int* mode ) const
{
	const auto& cfg = settings( );
	const auto remove = cfg.enabled && cfg.removals[ 0 ];
	if ( !output || !mode || !cfg.enabled || ( !cfg.fog_override && !remove ) )
		return false;

	const auto distance = std::clamp( cfg.fog_end, 100.0f, 20000.0f );
	const auto density = remove ? 0.0f : std::clamp( cfg.fog_density, 0.0f, 1.0f );
	const auto falloff = std::clamp( cfg.fog_exponent, 0.1f, 10.0f );
	const vec4 first{ 0.0f, distance, 0.0f, 0.0f };
	const vec4 second{ density, falloff, 0.0f, 0.0f };
	const vec4 third{ unit( cfg.fog_color.r ), unit( cfg.fog_color.g ), unit( cfg.fog_color.b ), 0.0f };
	world::set_shader_float( output, hash::gradient_fog, &first );
	world::set_shader_float( output, hash::gradient_fog_2, &second );
	world::set_shader_float( output, hash::gradient_fog_3, &third );
	world::set_shader_int( static_cast<std::uint8_t*>( output ) + 17 * 16, hash::enable_gradient_fog, remove ? 0 : 1 );
	*mode = 0;
	return true;
}

void ambience_t::on_shader_param( void*& value, std::uint32_t id ) const
{
	const auto& cfg = settings( );
	if ( !cfg.enabled )
		return;
	thread_local vec4 storage{};
	const auto set = [ & ]( float x, float y, float z, float w )
	{
		storage = { x, y, z, w };
		value = &storage;
	};

	if ( cfg.dof && id == hash::dof )
	{
		const auto near_plane = std::max( cfg.dof_near, 0.0f );
		const auto far_plane = std::max( cfg.dof_far, near_plane + 1.0f );
		set( 0.0f, near_plane, far_plane, far_plane * 1.6f );
		return;
	}

	const auto stripped = cfg.removals[ 2 ];
	if ( ( cfg.bloom >= 0.0f || stripped ) )
	{
		const auto t = stripped ? 0.0f : std::clamp( cfg.bloom, 0.0f, 1.0f );
		switch ( id )
		{
		case hash::bloom_scale: { const auto v = stripped ? 0.0f : 0.3f + t * 1.2f; set( v, v, v, v ); return; }
		case hash::bloom_threshold: { const auto v = 1.5f - t * 1.2f; set( v, v, v, v ); return; }
		case hash::bloom_width: { const auto v = 0.5f + t * 1.5f; set( v, v, v, v ); return; }
		case hash::bloom_strength: { const auto v = stripped ? 0.0f : 0.2f + t * 0.6f; set( v, v, v, v ); return; }
		case hash::bloom_skybox: { const auto v = stripped ? 0.0f : 0.1f + t * 0.4f; set( v, v, v, v ); return; }
		default: break;
		}
	}

	if ( cfg.gamma && id == hash::gamma )
	{
		const auto v = std::clamp( cfg.gamma_amount, 0.2f, 3.0f );
		set( v, v, v, v );
	}
}

} // namespace features::misc
