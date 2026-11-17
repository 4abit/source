#include <stdafx.hpp>

#include <features/misc/misc_extras.hpp>
#include <features/visuals/visuals.hpp>

#include <app/context.hpp>
#include <app/workers.hpp>
#include <config/settings.hpp>
#include <core/state/services.hpp>
#include <system/runtime_events.hpp>

#include <Windows.h>
#include <mmsystem.h>
#include <cmath>
#include <numbers>
#include <random>

namespace features::misc {
	namespace {

		// ---- hit sound -------------------------------------------------------------
		//
		// The tone is synthesised into a WAV image once per (style, volume) change and
		// played asynchronously, so no asset files are needed and the volume slider is
		// honoured by scaling the samples themselves.

		constexpr int k_sample_rate = 22050;

		struct tone
		{
			float frequency{};
			float secondary{};
			float decay{};
			float noise{};
			float noise_decay{};
		};

		[[nodiscard]] tone tone_for_style( const int style )
		{
			switch ( style )
			{
			case 1: return { 1800.0f, 2600.0f, 42.0f, 0.05f, 70.0f };  // glass
			case 2: return { 1180.0f, 1770.0f, 30.0f, 0.03f, 55.0f };  // pluck
			case 3: return { 2400.0f, 3200.0f, 65.0f, 0.02f, 90.0f };  // crisp
			case 4: return { 190.0f, 320.0f, 26.0f, 0.55f, 34.0f };    // flesh
			default: return { 880.0f, 1320.0f, 24.0f, 0.08f, 48.0f };   // soft
			}
		}

		void append_u32( std::vector<std::uint8_t>& out, const std::uint32_t value )
		{
			out.push_back( static_cast<std::uint8_t>( value & 0xFF ) );
			out.push_back( static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) );
			out.push_back( static_cast<std::uint8_t>( ( value >> 16 ) & 0xFF ) );
			out.push_back( static_cast<std::uint8_t>( ( value >> 24 ) & 0xFF ) );
		}

		void append_u16( std::vector<std::uint8_t>& out, const std::uint16_t value )
		{
			out.push_back( static_cast<std::uint8_t>( value & 0xFF ) );
			out.push_back( static_cast<std::uint8_t>( ( value >> 8 ) & 0xFF ) );
		}

		// 16-bit mono PCM inside a minimal RIFF/WAVE header.
		[[nodiscard]] std::vector<std::uint8_t> build_tone( const int style,
			const float volume )
		{
			const auto shape = tone_for_style( style );
			const auto duration = 0.16f;
			const auto frames = static_cast<std::size_t>(
				static_cast<float>( k_sample_rate ) * duration );

			std::vector<std::int16_t> samples( frames );
			std::mt19937 generator{ 0x51DE0u + static_cast<std::uint32_t>( style ) };
			std::uniform_real_distribution<float> noise( -1.0f, 1.0f );

			const auto level = std::clamp( volume, 0.0f, 1.0f );
			for ( std::size_t index = 0; index < frames; ++index )
			{
				const auto t = static_cast<float>( index )
					/ static_cast<float>( k_sample_rate );
				const auto envelope = std::exp( -shape.decay * t );
				auto value = std::sin( 2.0f * std::numbers::pi_v<float> * shape.frequency * t ) * 0.65f;
				value += std::sin( 2.0f * std::numbers::pi_v<float> * shape.secondary * t ) * 0.25f;
				value += noise( generator ) * shape.noise
					* std::exp( -shape.noise_decay * t );
				// A short attack avoids the click a hard start would produce.
				const auto attack = std::min( 1.0f, t * 400.0f );
				const auto scaled = std::clamp( value * envelope * attack * level,
					-1.0f, 1.0f );
				samples[ index ] = static_cast<std::int16_t>( scaled * 32000.0f );
			}

			std::vector<std::uint8_t> wav{};
			const auto data_bytes = static_cast<std::uint32_t>( samples.size( ) * 2 );
			wav.reserve( data_bytes + 44 );
			wav.insert( wav.end(), { 'R', 'I', 'F', 'F' } );
			append_u32( wav, data_bytes + 36 );
			wav.insert( wav.end(), { 'W', 'A', 'V', 'E' } );
			wav.insert( wav.end(), { 'f', 'm', 't', ' ' } );
			append_u32( wav, 16 );
			append_u16( wav, 1 );                                    // PCM
			append_u16( wav, 1 );                                    // mono
			append_u32( wav, static_cast<std::uint32_t>( k_sample_rate ) );
			append_u32( wav, static_cast<std::uint32_t>( k_sample_rate * 2 ) );
			append_u16( wav, 2 );                                    // block align
			append_u16( wav, 16 );                                   // bits
			wav.insert( wav.end(), { 'd', 'a', 't', 'a' } );
			append_u32( wav, data_bytes );
			const auto* raw = reinterpret_cast<const std::uint8_t*>( samples.data( ) );
			wav.insert( wav.end( ), raw, raw + data_bytes );
			return wav;
		}

		void play_hit_sound( const int style, const float volume )
		{
			// Rebuilt only when the settings move; PlaySound keeps the buffer alive for
			// the duration of the playback (SND_ASYNC), so it must not be freed early.
			static std::vector<std::uint8_t> image{};
			static int cached_style = -1;
			static float cached_volume = -1.0f;
			if ( style != cached_style || std::abs( volume - cached_volume ) > 0.01f )
			{
				image = build_tone( style, volume );
				cached_style = style;
				cached_volume = volume;
			}
			if ( image.empty( ) )
				return;
			::PlaySoundW( reinterpret_cast<LPCWSTR>( image.data( ) ), nullptr,
				SND_MEMORY | SND_ASYNC | SND_NODEFAULT );
		}

		// ---- input helpers ---------------------------------------------------------

		[[nodiscard]] bool key_down( const int virtual_key )
		{
			return virtual_key > 0 && virtual_key < 256
				&& ( ::GetAsyncKeyState( virtual_key ) & 0x8000 ) != 0;
		}

		[[nodiscard]] bool send_transition( input_lifecycle::owned_control& control,
			const game::input_action action, const bool pressed )
		{
			if ( !control.binding )
				control.binding = game::input_bindings( ).resolve( action );
			return input_lifecycle::transition( control, pressed,
				[]( const game::input_binding& binding, const bool down )
				{
					return app::context().input.key( binding.virtual_key, down );
				} );
		}

	} // namespace

	void misc_extras_t::tick( )
	{
		tick_hit_sound( );
		tick_quick_switch( );
		tick_afk_guard( );
		tick_knife_bot( );
		tick_grenade_assist( );
	}

	void misc_extras_t::tick_hit_sound( )
	{
		const auto snapshot = config::get_runtime_snapshot( );
		if ( !snapshot || !snapshot->general.m_hitsound.enabled )
			return;

		const auto hit = features::visuals::bullet_impacts( ).latest_confirmed_hit( );
		if ( hit.sequence == m_last_hit_sequence )
			return;
		m_last_hit_sequence = hit.sequence;
		if ( hit.damage <= 0 )
			return;

		// One sound per hit, but never twice inside the same frame.
		const auto now = std::chrono::steady_clock::now( );
		if ( now - m_last_sound < std::chrono::milliseconds( 15 ) )
			return;
		m_last_sound = now;

		play_hit_sound( snapshot->general.m_hitsound.style,
			snapshot->general.m_hitsound.volume );
	}

	void misc_extras_t::tick_quick_switch( )
	{
		const auto snapshot = config::get_runtime_snapshot( );
		if ( !snapshot )
			return;
		const auto& cfg = snapshot->general.m_quick_switch;

		const auto now = std::chrono::steady_clock::now( );
		const auto phase_age = now - m_quick_phase_started;

		if ( !cfg.enabled )
		{
			if ( m_quick_phase != 0 )
			{
				// Always hand the knife back before giving up the feature.
				static_cast<void>( send_transition( m_knife_attack, game::input_action::attack, false ) );
				m_quick_phase = 0;
			}
			return;
		}

		if ( m_quick_phase == 0 )
		{
			if ( !key_down( cfg.key ) )
				return;
			m_quick_slot = static_cast<std::uint16_t>( '3' ); // knife slot
			static_cast<void>( app::context().input.key( m_quick_slot, true ) );
			static_cast<void>( app::context().input.key( m_quick_slot, false ) );
			m_quick_phase = 1;
			m_quick_phase_started = now;
			return;
		}

		if ( m_quick_phase == 1 && phase_age > std::chrono::milliseconds( 60 ) )
		{
			// Back to the weapon that was held: the primary slot for now.
			static_cast<void>( app::context().input.key(
				static_cast<std::uint16_t>( '1' ), true ) );
			static_cast<void>( app::context().input.key(
				static_cast<std::uint16_t>( '1' ), false ) );
			m_quick_phase = 2;
			m_quick_phase_started = now;
			return;
		}

		if ( m_quick_phase == 2 && phase_age > std::chrono::milliseconds( 80 ) )
			m_quick_phase = 0;
	}

	void misc_extras_t::tick_afk_guard( )
	{
		const auto snapshot = config::get_runtime_snapshot( );
		if ( !snapshot || !snapshot->general.m_afk_guard.enabled )
			return;
		if ( !game::local_player( ).valid( ) || !game::local_player( ).alive( ) )
			return;

		const auto now = std::chrono::steady_clock::now( );
		const auto period = std::chrono::milliseconds( static_cast<int>(
			std::clamp( snapshot->general.m_afk_guard.interval, 10.0f, 120.0f ) * 1000.0f ) );
		if ( now - m_last_afk_tap < period )
			return;
		m_last_afk_tap = now;

		// A single tap of the forward key resets the idle timer; it is short enough
		// that the player barely moves.
		const auto forward = game::input_bindings( ).resolve( game::input_action::forward );
		if ( !forward )
			return;
		static_cast<void>( app::context().input.key( forward.virtual_key, true ) );
		std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
		static_cast<void>( app::context().input.key( forward.virtual_key, false ) );
	}

	void misc_extras_t::tick_knife_bot( )
	{
		const auto snapshot = config::get_runtime_snapshot( );
		if ( !snapshot )
			return;
		const auto& cfg = snapshot->general.m_knife_bot;

		if ( !cfg.enabled )
		{
			if ( m_knife_attack.pressed )
				static_cast<void>( send_transition( m_knife_attack, game::input_action::attack, false ) );
			return;
		}

		// Only while a knife is actually held, and only when the enemy is inside the
		// configured range and in front of the view. The weapon kind comes from the
		// item definition index: 42/59 are the stock knives, 500..526 the others.
		const auto held = game::local_player( ).weapon( );
		if ( !held )
			return;
		const auto definition = app::context().process.load<std::uint16_t>( held
			+ SCHEMA( "C_EconEntity", "m_AttributeManager"_id )
			+ SCHEMA( "C_AttributeContainer", "m_Item"_id )
			+ SCHEMA( "C_EconItemView", "m_iItemDefinitionIndex"_id ) );
		const bool knife = definition == 42 || definition == 59
			|| ( definition >= 500 && definition <= 526 );
		if ( !knife )
		{
			if ( m_knife_attack.pressed )
				static_cast<void>( send_transition( m_knife_attack, game::input_action::attack, false ) );
			return;
		}

		const auto pawn = game::local_player( ).pawn( );
		if ( !pawn || !game::local_player( ).alive( ) )
			return;
		const auto eye = game::camera( ).origin( );
		const auto view = game::camera( ).angles( );

		bool in_range = false;
		if ( const auto world = game::world( ).players( ) )
		{
			for ( const auto& player : *world )
			{
				if ( !game::local_player( ).is_enemy( player.team ) || player.health <= 0 )
					continue;
				const auto delta = player.collision_center - eye;
				const auto distance = delta.length( );
				if ( distance > std::max( cfg.range, 1.0f ) || distance < 0.1f )
					continue;
				const foundation::vec3 direction = delta * ( 1.0f / distance );
				foundation::vec3 forward{};
				view.to_directions( &forward, nullptr, nullptr );
				const auto dot = std::clamp( forward.dot( direction ), -1.0f, 1.0f );
				if ( std::acos( dot ) * 180.0f / std::numbers::pi_v<float> > cfg.fov )
					continue;
				in_range = true;
				break;
			}
		}

		const auto now = std::chrono::steady_clock::now( );
		if ( in_range )
		{
			// Alternate the two swings the way the knife's attack pattern expects.
			static_cast<void>( send_transition( m_knife_attack,
				cfg.swing_secondary ? game::input_action::attack2 : game::input_action::attack, true ) );
			if ( m_knife_attack_started.time_since_epoch( ).count( ) == 0 )
				m_knife_attack_started = now;
			if ( now - m_knife_attack_started > std::chrono::milliseconds( 120 ) )
			{
				static_cast<void>( send_transition( m_knife_attack,
					cfg.swing_secondary ? game::input_action::attack2 : game::input_action::attack, false ) );
				m_knife_attack_started = {};
			}
		}
		else if ( m_knife_attack.pressed )
		{
			static_cast<void>( send_transition( m_knife_attack,
				cfg.swing_secondary ? game::input_action::attack2 : game::input_action::attack, false ) );
			m_knife_attack_started = {};
		}
	}


	void misc_extras_t::tick_grenade_assist( )
	{
		const auto snapshot = config::get_runtime_snapshot( );
		if ( !snapshot )
			return;
		const auto& cfg = snapshot->general.m_grenade_assist;

		// Everything is released again when the feature goes away, so a disabled checkbox
		// can never leave the player stuck in a throw.
		const auto release_all = [ & ]
		{
			if ( m_grenade_attack.pressed )
				static_cast<void>( send_transition( m_grenade_attack, game::input_action::attack, false ) );
			if ( m_grenade_jump.pressed )
				static_cast<void>( send_transition( m_grenade_jump, game::input_action::jump, false ) );
			m_released = false;
		};
		if ( !cfg.auto_release && !cfg.super_toss )
		{
			release_all( );
			return;
		}
		if ( !game::local_player( ).valid( ) || !game::local_player( ).alive( ) )
		{
			release_all( );
			return;
		}

		// Only while a grenade is actually in hand and the throw is being primed: the
		// attack button is what the player holds before letting it fly.
		const auto held = game::local_player( ).weapon( );
		if ( !held )
			return;
		const auto definition = app::context().process.load<std::uint16_t>( held
			+ SCHEMA( "C_EconEntity", "m_AttributeManager"_id )
			+ SCHEMA( "C_AttributeContainer", "m_Item"_id )
			+ SCHEMA( "C_EconItemView", "m_iItemDefinitionIndex"_id ) );
		// 43 high explosive, 44 flashbang, 45 smoke, 46 molotov, 47 decoy, 48 incendiary.
		const bool grenade = definition >= 43 && definition <= 48;
		if ( !grenade )
		{
			release_all( );
			return;
		}

		const auto attack = game::input_bindings( ).resolve( game::input_action::attack );
		const bool priming = attack && ( ::GetAsyncKeyState( attack.virtual_key ) & 0x8000 ) != 0;
		if ( !priming && !m_released )
			return;

		// Ballistic preview of the throw: the same gravity the game uses, stepped until the
		// first surface. It is an estimate, not the engine's own prediction, so the trigger
		// threshold is expressed as a distance in the menu.
		const auto eye = game::camera( ).origin( );
		const auto angles = game::camera( ).angles( );
		foundation::vec3 forward{};
		angles.to_directions( &forward, nullptr, nullptr );
		constexpr float throw_speed = 750.0f;
		constexpr float gravity = 800.0f;
		foundation::vec3 position = eye;
		foundation::vec3 velocity = forward * throw_speed;
		foundation::vec3 landing = eye;
		constexpr float step = 0.02f;
		for ( int index = 0; index < 150; ++index )
		{
			const auto next = position + velocity * step;
			velocity.z -= gravity * step;
			if ( game::collision( ).valid( ) )
			{
				const auto trace = game::collision( ).trace_ray( position, next );
				if ( trace.hit )
				{
					landing = trace.end_pos;
					break;
				}
			}
			position = next;
			landing = position;
		}

		// Distance to the nearest live enemy from the landing point, turned into the
		// familiar linear falloff over the high explosive radius.
		constexpr float blast_radius = 350.0f;
		float nearest = blast_radius;
		if ( const auto world = game::world( ).players( ) )
		{
			for ( const auto& player : *world )
			{
				if ( !game::local_player( ).is_enemy( player.team ) || player.health <= 0 )
					continue;
				nearest = std::min( nearest, ( player.collision_center - landing ).length( ) );
			}
		}
		const auto damage = 100.0f * std::max( 0.0f, 1.0f - nearest / blast_radius );

		if ( !cfg.auto_release || damage < cfg.min_damage )
			return;

		// Super Toss: a jump pressed into the release adds the throw the extra reach.
		const auto now = std::chrono::steady_clock::now( );
		if ( cfg.super_toss && now - m_last_jump > std::chrono::milliseconds( 250 ) )
		{
			m_last_jump = now;
			static_cast<void>( send_transition( m_grenade_jump, game::input_action::jump, true ) );
		}
		if ( m_grenade_jump.pressed && now - m_last_jump > std::chrono::milliseconds( 40 ) )
			static_cast<void>( send_transition( m_grenade_jump, game::input_action::jump, false ) );

		// Let the throw fly, then hand the button straight back.
		static_cast<void>( send_transition( m_grenade_attack, game::input_action::attack, false ) );
		m_release_started = now;
		m_released = true;
		platform::windows::runtime_event( "grenade",
			std::format( "released def={} drop={:.0f} damage={:.0f}", definition, nearest, damage ) );
	}
} // namespace features::misc
