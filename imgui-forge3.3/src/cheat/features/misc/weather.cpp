#include <stdafx.hpp>

#include <features/misc/weather.hpp>
#include <features/visuals/visuals.hpp>
#include <features/world/world_hooks.hpp>

#include <app/context.hpp>
#include <config/visuals.hpp>
#include <core/state/services.hpp>

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>

namespace features::misc {
namespace {

	namespace hash {
		constexpr std::uint32_t wind_direction = 0x2A416C12;
		constexpr std::uint32_t wind_strength_frequency = 0xEB0D997E;
		constexpr std::uint32_t rain_exposure_to_sky = 0x374C1B3C;
		constexpr std::uint32_t rain_timer = 0x2DBEE393;
		constexpr std::uint32_t rain_wetness = 0x0F592812;
	}

	constexpr const char* k_rain = "particles/embedded/rain.vpcf";
	constexpr const char* k_snow = "particles/embedded/snow.vpcf";
	constexpr const char* k_stars = "particles/embedded/stars.vpcf";
	constexpr const char* k_tracer = "particles/embedded/tracer.vpcf";
	constexpr const char* k_sparks = "particles/embedded/sparks.vpcf";

	struct alignas( 16 ) vec4
	{
		float x, y, z, w;
	};

	[[nodiscard]] float seconds()
	{
		using clock = std::chrono::steady_clock;
		static const auto start = clock::now();
		return std::chrono::duration<float>( clock::now() - start ).count();
	}

	[[nodiscard]] const auto& settings()
	{
		return config::visual_settings.m_weather;
	}

	[[nodiscard]] foundation::vec3 color_of( const zdraw::rgba& color )
	{
		return { static_cast<float>( color.r ), static_cast<float>( color.g ), static_cast<float>( color.b ) };
	}

	[[nodiscard]] foundation::vec3 origin_of( std::uintptr_t pawn )
	{
		const auto& process = app::context().process;
		const auto node = process.load<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_id ) );
		return node ? process.load<foundation::vec3>( node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_id ) )
			: foundation::vec3{};
	}

	// Horizontal wind vector in 0..1, with slow gusts layered on top.
	void wind_vector( float& x, float& y )
	{
		const auto& cfg = settings();
		x = y = 0.0f;
		if ( !cfg.wind_enabled )
			return;
		const auto direction = foundation::to_radians( cfg.wind_direction );
		const auto strength = std::clamp( cfg.wind_strength / 5.0f, 0.0f, 2.0f );
		const auto gusts = std::clamp( cfg.wind_turbulence / 5.0f, 0.0f, 1.0f );
		x = strength * std::cos( direction );
		y = strength * std::sin( direction );
		if ( gusts > 0.0f )
		{
			const auto t = seconds() * 0.6f;
			x += gusts * 0.9f * ( 0.6f * std::sin( t * 0.9f + 0.3f ) + 0.3f * std::sin( t * 2.3f + 1.7f ) + 0.15f * std::sin( t * 5.1f + 4.2f ) );
			y += gusts * 0.9f * ( 0.6f * std::sin( t * 1.1f + 2.0f ) + 0.3f * std::sin( t * 2.7f + 0.5f ) + 0.15f * std::sin( t * 4.6f + 3.1f ) );
		}
	}

	[[nodiscard]] float random_unit( std::uint32_t& seed )
	{
		seed = seed * 1664525u + 1013904223u;
		return static_cast<float>( seed >> 8 ) / static_cast<float>( 1u << 24 );
	}

	void append_u32( std::vector<std::uint8_t>& out, std::uint32_t value )
	{
		for ( int i = 0; i < 4; ++i )
			out.push_back( static_cast<std::uint8_t>( value >> ( i * 8 ) ) );
	}

	// A crack followed by a long low rumble: brown noise through a falling
	// low-pass, with a couple of later rolls.
	[[nodiscard]] std::vector<std::uint8_t> thunder_wave( float volume, float distance, std::uint32_t seed )
	{
		constexpr int rate = 22050;
		const auto remote = std::clamp( distance / 2500.0f, 0.0f, 1.0f );
		const auto duration = 2.8f + remote * 1.4f;
		const auto frames = static_cast<std::size_t>( rate * duration );
		std::vector<std::int16_t> samples( frames );
		std::mt19937 generator{ seed };
		std::uniform_real_distribution<float> noise( -1.0f, 1.0f );
		float brown = 0.0f, low = 0.0f;
		const auto roll_a = 0.6f + noise( generator ) * 0.2f;
		const auto roll_b = 1.4f + noise( generator ) * 0.3f;
		for ( std::size_t i = 0; i < frames; ++i )
		{
			const auto t = static_cast<float>( i ) / rate;
			brown = std::clamp( brown + noise( generator ) * 0.08f, -1.0f, 1.0f ) * 0.996f;
			const auto cutoff = 0.02f + ( 1.0f - remote ) * 0.25f * std::exp( -t * 4.0f );
			low += ( brown - low ) * cutoff;
			auto envelope = std::min( 1.0f, t * 30.0f ) * std::exp( -t * 1.1f );
			envelope += 0.6f * std::exp( -std::pow( ( t - roll_a ) * 3.0f, 2.0f ) );
			envelope += 0.4f * std::exp( -std::pow( ( t - roll_b ) * 2.0f, 2.0f ) );
			auto value = low * 3.2f * envelope;
			if ( t < 0.12f && remote < 0.6f )
				value += noise( generator ) * ( 1.0f - remote ) * std::exp( -t * 35.0f ) * 0.8f;
			samples[ i ] = static_cast<std::int16_t>( std::clamp( value * volume, -1.0f, 1.0f ) * 30000.0f );
		}

		std::vector<std::uint8_t> wav{};
		const auto bytes = static_cast<std::uint32_t>( samples.size() * 2 );
		wav.reserve( bytes + 44 );
		wav.insert( wav.end(), { 'R', 'I', 'F', 'F' } );
		append_u32( wav, bytes + 36 );
		wav.insert( wav.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' } );
		append_u32( wav, 16 );
		wav.insert( wav.end(), { 1, 0, 1, 0 } );
		append_u32( wav, rate );
		append_u32( wav, rate * 2 );
		wav.insert( wav.end(), { 2, 0, 16, 0 } );
		wav.insert( wav.end(), { 'd', 'a', 't', 'a' } );
		append_u32( wav, bytes );
		const auto* raw = reinterpret_cast<const std::uint8_t*>( samples.data() );
		wav.insert( wav.end(), raw, raw + bytes );
		return wav;
	}

	void beam( const foundation::vec3& from, const foundation::vec3& to, const foundation::vec3& color, float life )
	{
		const auto effect = world::particles::create( k_tracer );
		if ( effect == world::particles::invalid )
			return;
		world::particles::control_point( effect, 0, from );
		world::particles::control_point( effect, 1, to );
		world::particles::control_point( effect, 2, color );
		world::particles::control_point( effect, 3, { life, 0.0f, 0.0f } );
	}

} // namespace

void weather_t::update_layer( layer& slot, bool wanted, const char* path, const foundation::vec3& origin,
	const foundation::vec3& color, bool lean )
{
	if ( !wanted )
	{
		if ( slot.effect != world::particles::invalid )
			world::particles::destroy( slot.effect );
		slot.effect = world::particles::invalid;
		return;
	}
	if ( slot.effect == world::particles::invalid )
		slot.effect = world::particles::create( path );
	if ( slot.effect == world::particles::invalid )
		return;

	float wind_x = 0.0f, wind_y = 0.0f;
	if ( lean )
		wind_vector( wind_x, wind_y );
	const auto magnitude = std::min( std::sqrt( wind_x * wind_x + wind_y * wind_y ), 1.0f );
	if ( magnitude > 0.01f )
	{
		const auto tilt = foundation::to_radians( magnitude * 70.0f );
		const auto heading = std::atan2( wind_y, wind_x );
		const auto sine = std::sin( tilt * 0.5f );
		const float rotation[ 4 ]{ std::sin( heading ) * sine, -std::cos( heading ) * sine, 0.0f, std::cos( tilt * 0.5f ) };
		world::particles::transform( slot.effect, 0, origin, rotation );
	}
	else
		world::particles::control_point( slot.effect, 0, origin );
	world::particles::control_point( slot.effect, 1, color );
}

void weather_t::release_all()
{
	for ( auto* slot : { &m_rain, &m_snow, &m_stars } )
	{
		if ( slot->effect != world::particles::invalid )
			world::particles::destroy( slot->effect );
		slot->effect = world::particles::invalid;
	}
}

void weather_t::on_frame()
{
	const auto& cfg = settings();
	const auto now = seconds();

	const auto age = now - m_flash_start;
	auto level = 0.0f;
	if ( cfg.lightning_enabled && cfg.lightning_flash && age >= 0.0f && age < 0.9f )
	{
		// Two flickers and a fading afterglow, the way a real strike reads.
		const auto first = std::exp( -std::pow( ( age - 0.03f ) * 22.0f, 2.0f ) );
		const auto second = 0.8f * std::exp( -std::pow( ( age - 0.16f ) * 18.0f, 2.0f ) );
		const auto glow = 0.35f * std::exp( -age * 4.0f );
		level = std::clamp( first + second + glow, 0.0f, 1.0f );
	}
	m_flash.store( level, std::memory_order_relaxed );

	if ( !world::particles::ready() )
		return;

	const auto generation = world::particles::generation();
	if ( generation != m_generation )
	{
		// The manager or the round was rebuilt: our indices point at nothing now.
		m_rain = {};
		m_snow = {};
		m_stars = {};
		m_generation = generation;
	}

	const auto pawn = game::local_player().pawn();
	if ( !pawn )
	{
		release_all();
		return;
	}
	const auto origin = origin_of( pawn );

	update_layer( m_rain, cfg.rain_enabled, k_rain, origin, color_of( cfg.rain_color ), true );
	update_layer( m_snow, cfg.snow_enabled, k_snow, origin, color_of( cfg.snow_color ), true );
	update_layer( m_stars, cfg.stars_enabled, k_stars, origin, color_of( cfg.stars_color ), false );

	if ( !cfg.lightning_enabled )
	{
		m_thunder.clear();
		return;
	}

	if ( cfg.lightning_spawn_on_kill )
	{
		const auto hit = features::visuals::bullet_impacts().latest_confirmed_hit();
		if ( hit.sequence != m_last_kill_sequence )
		{
			m_last_kill_sequence = hit.sequence;
			if ( hit.killed && hit.pawn )
				strike( origin_of( hit.pawn ), true );
		}
	}
	else if ( now >= m_next_strike )
	{
		const auto interval = std::clamp( cfg.lightning_interval, 1.0f, 60.0f );
		m_next_strike = now + interval * ( 0.6f + random_unit( m_seed ) * 0.8f );
		strike( origin, false );
	}

	for ( auto it = m_thunder.begin(); it != m_thunder.end(); )
	{
		if ( now < it->at )
		{
			++it;
			continue;
		}
		play_thunder( it->distance );
		it = m_thunder.erase( it );
	}
}

void weather_t::strike( const foundation::vec3& around, bool exact )
{
	const auto& cfg = settings();
	auto ground = around;
	auto distance = 0.0f;
	if ( !exact )
	{
		const auto angle = random_unit( m_seed ) * 2.0f * std::numbers::pi_v<float>;
		distance = 500.0f + random_unit( m_seed ) * 1400.0f;
		ground = { around.x + std::cos( angle ) * distance, around.y + std::sin( angle ) * distance, around.z };
		if ( game::collision().valid() )
		{
			const foundation::vec3 high{ ground.x, ground.y, around.z + 512.0f };
			const auto trace = game::collision().trace_ray( high, { ground.x, ground.y, around.z - 1500.0f } );
			if ( trace.hit )
				ground = trace.end_pos;
		}
	}

	const auto color = color_of( cfg.lightning_color );
	const auto life = std::clamp( cfg.lightning_duration, 0.15f, 1.5f ) + 0.2f;
	constexpr int segments = 14;
	constexpr float height = 2600.0f;
	std::vector<foundation::vec3> trunk{};
	trunk.reserve( segments + 1 );
	foundation::vec3 point{ ground.x, ground.y, ground.z + height };
	trunk.push_back( point );
	for ( int i = 1; i < segments; ++i )
	{
		const auto wander = 120.0f * std::sin( static_cast<float>( i ) / segments * std::numbers::pi_v<float> );
		point.x += ( random_unit( m_seed ) * 2.0f - 1.0f ) * wander;
		point.y += ( random_unit( m_seed ) * 2.0f - 1.0f ) * wander;
		point.z = ground.z + height * ( 1.0f - static_cast<float>( i ) / segments );
		trunk.push_back( point );
	}
	trunk.push_back( ground );
	for ( std::size_t i = 1; i < trunk.size(); ++i )
		beam( trunk[ i - 1 ], trunk[ i ], color, life );

	const auto forks = 2 + static_cast<int>( random_unit( m_seed ) * 3.0f );
	for ( int fork = 0; fork < forks; ++fork )
	{
		auto from = trunk[ 2 + static_cast<std::size_t>( random_unit( m_seed ) * ( segments - 5 ) ) ];
		const auto steps = 2 + static_cast<int>( random_unit( m_seed ) * 3.0f );
		for ( int i = 0; i < steps; ++i )
		{
			const foundation::vec3 to{ from.x + ( random_unit( m_seed ) * 2.0f - 1.0f ) * 160.0f,
				from.y + ( random_unit( m_seed ) * 2.0f - 1.0f ) * 160.0f,
				from.z - 110.0f - random_unit( m_seed ) * 140.0f };
			beam( from, to, color, life * 0.7f );
			from = to;
		}
	}

	const auto sparks = world::particles::create( k_sparks );
	if ( sparks != world::particles::invalid )
	{
		world::particles::control_point( sparks, 0, ground );
		world::particles::control_point( sparks, 1, color );
	}

	m_flash_start = seconds();
	if ( cfg.lightning_sound )
		m_thunder.push_back( { m_flash_start + std::clamp( distance / 3400.0f, 0.05f, 2.5f ), distance } );
}

void weather_t::play_thunder( float distance ) const
{
	static std::vector<std::uint8_t> buffers[ 2 ]{};
	static int next = 0;
	const auto volume = std::clamp( settings().thunder_volume, 0.0f, 1.0f );
	if ( volume <= 0.0f )
		return;
	auto& wave = buffers[ next ];
	next ^= 1;
	wave = thunder_wave( volume, distance, m_seed ^ static_cast<std::uint32_t>( seconds() * 1000.0f ) );
	::PlaySoundW( reinterpret_cast<LPCWSTR>( wave.data() ), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT );
}

void weather_t::on_shader_param( void*& value, std::uint32_t id ) const
{
	const auto& cfg = settings();
	thread_local vec4 storage{};
	const auto set = [ & ]( float x, float y, float z, float w )
	{
		storage = { x, y, z, w };
		value = &storage;
	};

	if ( cfg.wind_enabled )
	{
		if ( id == hash::wind_direction )
		{
			float x = 0.0f, y = 0.0f;
			wind_vector( x, y );
			const auto length = std::max( std::sqrt( x * x + y * y ), 0.001f );
			set( x / length, y / length, 0.0f, 0.0f );
			return;
		}
		if ( id == hash::wind_strength_frequency )
		{
			const auto strength = std::clamp( cfg.wind_strength, 0.0f, 10.0f );
			const auto gusts = std::clamp( cfg.wind_turbulence, 0.0f, 5.0f );
			set( strength, gusts, strength, gusts );
			return;
		}
	}

	if ( cfg.wetness_enabled )
	{
		if ( id == hash::rain_exposure_to_sky )
			set( 1.0f, 1.0f, 1.0f, 1.0f );
		else if ( id == hash::rain_wetness )
		{
			const auto v = std::clamp( cfg.wetness_density, 0.0f, 4.0f );
			set( v, v, v, v );
		}
		else if ( id == hash::rain_timer )
		{
			const auto v = seconds() * std::clamp( cfg.wetness_speed, 0.0f, 4.0f );
			set( v, v, v, v );
		}
	}
}

} // namespace features::misc
