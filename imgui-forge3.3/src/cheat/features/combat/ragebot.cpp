#include <stdafx.hpp>

#include <features/combat/ragebot.hpp>
#include <features/combat/usercmd.hpp>

#include <core/hooking/trampoline.hpp>
#include <simulation/ballistics.hpp>
#include <simulation/punch_snapshot.hpp>
#include <core/math/ray_capsule.hpp>
#include <core/input/bindings.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace features::combat {
namespace {

	constexpr float k_zeus_range = 183.0f;

	struct command_wish
	{
		// Anti-aim angles for commands that do not shoot.
		std::atomic<bool> replace{ false };
		std::atomic<float> pitch{ 0.0f };
		std::atomic<float> yaw{ 0.0f };
		std::atomic<bool> skip_on_use{ false };

		// Silent aim: angles every shooting command leaves with, and a one-shot
		// request to press attack inside the next command.
		std::atomic<bool> shot{ false };
		std::atomic<float> shot_pitch{ 0.0f };
		std::atomic<float> shot_yaw{ 0.0f };
		std::atomic<bool> fire{ false };
		std::atomic<std::uint32_t> fired{ 0 };

		std::atomic<bool> aim{ false };
		std::atomic<float> aim_pitch{ 0.0f };
		std::atomic<float> aim_yaw{ 0.0f };
		std::atomic<float> aim_blend{ 1.0f };

		// Quick stop: every grounded command walks against the velocity.
		std::atomic<bool> stop{ false };
	};

	struct marker
	{
		bool draw{};
		bool returning{};
		foundation::vec3 origin{};
		float color[ 4 ]{ 0.70f, 0.83f, 1.0f, 1.0f };
	};

	struct candidate
	{
		const game::player_snapshot* player{};
		foundation::vec3 point{};
		foundation::vec3 angles{};
		int hitgroup{ -1 };
		float damage{};
		float fov{};
		float chance{ 1.0f };
		bool penetrated{};
	};

	struct runtime
	{
		hooking::trampoline create_move{};
		hooking::trampoline handle_view{};
		std::uintptr_t create_move_target{};
		command_wish command{};
		std::mutex marker_lock{};
		marker peek_marker{};

		bool was_enabled{};
		bool attack_down{};
		bool scope_down{};
		bool duck_down{};
		bool returning{};
		bool peek_saved{};
		bool second_shot{};
		bool queued_second{};
		foundation::vec3 peek_origin{};
		std::vector<std::uint16_t> owned_move{};
		std::uintptr_t tracked_pawn{};
		std::chrono::steady_clock::time_point tracked_since{};
		std::chrono::steady_clock::time_point kill_until{};
		std::chrono::steady_clock::time_point shot_until{};
		std::chrono::steady_clock::time_point attack_down_at{};
		int shot_clip{ -1 };
		foundation::vec3 shot_angles{};
		std::chrono::steady_clock::time_point shot_hold_until{};
		bool fire_pending{};
		std::uint32_t seen_fired{};
		std::chrono::steady_clock::time_point fire_requested_at{};
		std::chrono::steady_clock::time_point fire_block_until{};
		int tracked_health{};
		float spin_clock{};
		float last_view_yaw{};
		bool view_yaw_valid{};
		bool fps_spoofed{};
		float saved_fps{ -1.0f };
		std::uint32_t rng{ 0xA341316Cu };
	};

	runtime& state()
	{
		static runtime value{};
		return value;
	}

	using create_move_fn = void( __fastcall* )( void*, int, bool );

	[[nodiscard]] float next_unit( runtime& self )
	{
		self.rng = self.rng * 1664525u + 1013904223u;
		return static_cast<float>( self.rng >> 8 ) / static_cast<float>( 1u << 24 );
	}

	[[nodiscard]] bool finite_vec( const foundation::vec3& value )
	{
		return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z );
	}

	[[nodiscard]] bool code_pointer( std::uintptr_t address )
	{
		const auto client = app::context().process.module_base( "client.dll" );
		const auto size = app::context().process.module_image_size( client );
		if ( !client || !size || address < client || address >= client + size )
			return false;
		std::uint8_t bytes[ 4 ]{};
		if ( !app::context().process.copy( address, bytes, sizeof( bytes ) ) )
			return false;
		// int3 / zero padding means the slot does not point at a function start.
		return bytes[ 0 ] != 0xCC && bytes[ 0 ] != 0x00 && bytes[ 0 ] != 0xC3;
	}

	// The input object's function table: +0x28 builds the command
	// (create_move), +0x40 applies the command angles to the view.
	[[nodiscard]] std::uintptr_t input_function( std::uintptr_t slot )
	{
		const auto client = app::context().process.module_base( "client.dll" );
		const auto site = client ? app::context().process.scan_signature( client,
			"FF FF FF FF 48 8D 05 ? ? ? ? 48 89 0D ? ? ? ?" ) : 0;
		if ( !site )
			return 0;
		const auto global = app::context().process.decode_rip( site + 4, 3, 7 );
		if ( !global )
			return 0;
		const auto function = app::context().process.load<std::uintptr_t>( global + slot );
		return code_pointer( function ) ? function : 0;
	}

	[[nodiscard]] std::uintptr_t find_create_move() { return input_function( 0x28 ); }
	[[nodiscard]] std::uintptr_t find_handle_view() { return input_function( 0x40 ); }

	void steer_view( command_wish& wish )
	{
		if ( !wish.aim.load( std::memory_order_relaxed ) )
			return;
		foundation::vec3 view{};
		if ( !usercmd::view_angles( view ) )
			return;
		const auto blend = std::clamp( wish.aim_blend.load( std::memory_order_relaxed ), 0.01f, 1.0f );
		const auto pitch = wish.aim_pitch.load( std::memory_order_relaxed );
		const auto yaw = wish.aim_yaw.load( std::memory_order_relaxed );
		view.x += ( pitch - view.x ) * blend;
		view.y = foundation::wrap_yaw( view.y + foundation::wrap_yaw( yaw - view.y ) * blend );
		( void )usercmd::set_view_angles( view );
	}

	std::atomic<std::uint64_t> g_move_calls{};
	std::atomic<std::uint64_t> g_move_writes{};
	std::atomic<std::uint64_t> g_move_no_cmd{};
	std::atomic<std::uint64_t> g_move_shots{};
	std::atomic<std::uint64_t> g_view_restores{};
	// Set while the last command left with replaced angles, so the view hook
	// knows the angles it is about to apply are not the player's.
	std::atomic<bool> g_angles_replaced{ false };
	std::atomic<bool> g_bhop_armed{ false };
	std::atomic<bool> g_bhop_hold{ false };
	std::atomic<float> g_written_pitch{ 0.0f };
	std::atomic<float> g_written_yaw{ 0.0f };

	[[nodiscard]] float angle_gap( const foundation::vec3& a, const foundation::vec3& b )
	{
		return std::abs( a.x - b.x ) + std::abs( foundation::wrap_yaw( a.y - b.y ) );
	}

	using handle_view_fn = void( __fastcall* )( void*, int );

	// The client copies the command angles into the view here. With anti-aim
	// that turned the camera with the fake yaw (and, once the mouse moved, kept
	// it there: the next fake yaw was built on top of it, a 180 degree flip).
	// The leaked part is taken back out; anything else the call changed (a
	// server-forced angle) is kept relative to what the player aimed at.
	void __fastcall handle_view_hook( void* self, int slot )
	{
		auto& runtime = state();
		foundation::vec3 saved{};
		const auto restore = g_angles_replaced.load( std::memory_order_relaxed )
			&& usercmd::view_angles( saved );
		runtime.handle_view.original<handle_view_fn>()( self, slot );
		if ( !restore )
			return;
		foundation::vec3 now{};
		if ( !usercmd::view_angles( now ) || angle_gap( now, saved ) < 1.0e-4f )
			return;
		const foundation::vec3 written{ g_written_pitch.load( std::memory_order_relaxed ),
			g_written_yaw.load( std::memory_order_relaxed ), 0.0f };
		if ( angle_gap( now, written ) >= angle_gap( now, saved ) )
			return;
		const foundation::vec3 fixed{
			std::clamp( saved.x + ( now.x - written.x ), -89.0f, 89.0f ),
			foundation::wrap_yaw( saved.y + foundation::wrap_yaw( now.y - written.y ) ), 0.0f };
		( void )usercmd::set_view_angles( fixed );
		g_view_restores.fetch_add( 1, std::memory_order_relaxed );
	}

	// Fallback for when the view hook could not be installed: command angles
	// that leaked into the view are taken back out, keeping whatever the mouse
	// added since (a plain equality test lost the fix on the first mouse move).
	struct view_guard
	{
		bool valid{};
		foundation::vec3 real{};
		foundation::vec3 written{};
	};

	void undo_leak( view_guard& guard )
	{
		if ( !guard.valid )
			return;
		guard.valid = false;
		foundation::vec3 view{};
		if ( !usercmd::view_angles( view ) || angle_gap( guard.written, guard.real ) < 0.5f )
			return;
		if ( angle_gap( view, guard.written ) >= angle_gap( view, guard.real ) )
			return;
		const foundation::vec3 fixed{
			std::clamp( guard.real.x + ( view.x - guard.written.x ), -89.0f, 89.0f ),
			foundation::wrap_yaw( guard.real.y + foundation::wrap_yaw( view.y - guard.written.y ) ), 0.0f };
		( void )usercmd::set_view_angles( fixed );
	}

	void __fastcall create_move_hook( void* self, int slot, bool active )
	{
		auto& runtime = state();
		auto& wish = runtime.command;
		static view_guard guard{};
		g_move_calls.fetch_add( 1, std::memory_order_relaxed );
		( void )usercmd::resolve();

		const auto view_hooked = runtime.handle_view.active();
		if ( !view_hooked )
			undo_leak( guard );
		guard.valid = false;
		steer_view( wish );

		// Ground state at the start of this command, i.e. what the previous command
		// was predicted to end on. Read after the original, the flags already hold
		// this command's own prediction: a predicted jump reads as airborne and the
		// jump was taken back on the very tick it had to go out.
		const auto pawn = game::local_player().pawn();
		const auto flags_at = pawn ? pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_id ) : 0;
		const auto grounded_at_start = flags_at
			&& ( *reinterpret_cast<const std::uint32_t*>( flags_at ) & 1u ) != 0u;

		runtime.create_move.original<create_move_fn>()( self, slot, active );

		const auto controller = game::local_player().controller();
		const auto cmd = usercmd::ready() ? usercmd::current( controller ) : 0;
		std::uint64_t held{};
		const auto have_buttons = cmd && usercmd::buttons( cmd, held );
		const auto attacking = have_buttons && ( held & usercmd::in_attack ) != 0u;
		const auto using_key = have_buttons && ( held & usercmd::in_use ) != 0u;

		usercmd::write_options options{};
		options.grounded = flags_at
			&& ( *reinterpret_cast<const std::uint32_t*>( flags_at ) & 1u ) != 0u;

		// Bunny hop: jump is let go in the air and pressed again on the first
		// command that starts on the ground, the earliest tick the server
		// accepts a new jump.
		static bool jump_sent = false;
		static bool jump_edited = false;
		if ( cmd && have_buttons )
		{
			const auto user_jump = ( held & usercmd::in_jump ) != 0u;
			auto final_jump = user_jump;
			auto edit = jump_edited;
			if ( g_bhop_armed.load( std::memory_order_relaxed ) && pawn
				&& ( user_jump || g_bhop_hold.load( std::memory_order_relaxed ) ) )
			{
				const auto move_type = *reinterpret_cast<const std::uint8_t*>( pawn
					+ SCHEMA( "C_BaseEntity", "m_nActualMoveType"_id ) );
				if ( move_type != 7 && move_type != 8 && move_type != 9 )
				{
					final_jump = grounded_at_start;
					edit = true;
				}
			}
			if ( edit )
				( void )usercmd::set_button( cmd, usercmd::in_jump, final_jump, jump_sent );
			jump_edited = edit && final_jump != user_jump;
			jump_sent = final_jump;
		}

		// Quick stop inside the command: the move points against the velocity at
		// full input, then goes to zero once one tick of counter acceleration
		// plus friction (about 25 u/s standing) would overshoot. Done before the
		// angles are written so the move is rotated along with them. Pressing the
		// opposite key from outside did nothing while the player held his own:
		// W and S together cancel out and only friction slowed him down.
		foundation::vec3 stop_view{};
		if ( cmd && grounded_at_start && wish.stop.load( std::memory_order_relaxed )
			&& usercmd::view_angles( stop_view ) )
		{
			const auto velocity = *reinterpret_cast<const foundation::vec3*>( pawn
				+ SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_id ) );
			const auto speed = std::hypot( velocity.x, velocity.y );
			float forward = 0.0f;
			float left = 0.0f;
			if ( std::isfinite( speed ) && speed > 25.0f )
			{
				const auto yaw = foundation::to_radians( stop_view.y );
				const auto x = -velocity.x / speed;
				const auto y = -velocity.y / speed;
				forward = x * std::cos( yaw ) + y * std::sin( yaw );
				left = -x * std::sin( yaw ) + y * std::cos( yaw );
			}
			( void )usercmd::set_move( cmd, forward, left );
		}

		auto wrote = false;
		foundation::vec3 written{};
		const auto fire = wish.fire.load( std::memory_order_relaxed );
		if ( cmd && wish.shot.load( std::memory_order_relaxed ) && ( fire || attacking ) )
		{
			// Silent aim: whatever command shoots carries the target angles.
			written = { wish.shot_pitch.load( std::memory_order_relaxed ),
				wish.shot_yaw.load( std::memory_order_relaxed ), 0.0f };
			options.attack = fire;
			wrote = usercmd::write_angles( cmd, written.x, written.y, options );
			if ( wrote && fire )
			{
				wish.fire.store( false, std::memory_order_relaxed );
				wish.fired.fetch_add( 1, std::memory_order_relaxed );
				g_move_shots.fetch_add( 1, std::memory_order_relaxed );
			}
		}
		else if ( cmd && wish.replace.load( std::memory_order_relaxed ) && !attacking
			&& !( using_key && wish.skip_on_use.load( std::memory_order_relaxed ) ) )
		{
			// Anti-aim never rides on a shot the player fires himself (it would
			// send the bullet backwards) nor on +use.
			written = { wish.pitch.load( std::memory_order_relaxed ),
				wish.yaw.load( std::memory_order_relaxed ), 0.0f };
			wrote = usercmd::write_angles( cmd, written.x, written.y, options );
		}
		else if ( wish.replace.load( std::memory_order_relaxed ) && !cmd )
			g_move_no_cmd.fetch_add( 1, std::memory_order_relaxed );

		if ( wrote )
		{
			g_written_pitch.store( std::clamp( written.x, -89.0f, 89.0f ), std::memory_order_relaxed );
			g_written_yaw.store( foundation::wrap_yaw( written.y ), std::memory_order_relaxed );
		}
		g_angles_replaced.store( wrote, std::memory_order_relaxed );
		if ( wrote )
		{
			g_move_writes.fetch_add( 1, std::memory_order_relaxed );
			foundation::vec3 real{};
			if ( !view_hooked && usercmd::view_angles( real ) )
				guard = { true, real, { std::clamp( written.x, -89.0f, 89.0f ), foundation::wrap_yaw( written.y ), 0.0f } };
		}
		usercmd::publish_view_angles();
	}

	void install_handle_view()
	{
		static int attempts = 0;
		if ( state().handle_view.active() || attempts >= 5 )
			return;
		++attempts;
		const auto function = find_handle_view();
		if ( !function || function == state().create_move_target
			|| !state().handle_view.install( reinterpret_cast<void*>( function ),
				reinterpret_cast<void*>( &handle_view_hook ) ) )
		{
			if ( attempts == 5 )
				app::context().diagnostics.warning(
					"[rage] view hook was not installed; the camera falls back to the leak guard." );
			return;
		}
		app::context().diagnostics.info( "[rage] view hook installed." );
	}

	void install_create_move()
	{
		constexpr int k_max_attempts = 5;
		static int attempts = 0;
		static auto next_attempt = std::chrono::steady_clock::time_point{};
		if ( state().create_move.active() )
		{
			if ( !state().handle_view.active() )
				install_handle_view();
			return;
		}
		if ( attempts >= k_max_attempts )
			return;
		const auto now = std::chrono::steady_clock::now();
		if ( now < next_attempt )
			return;
		next_attempt = now + std::chrono::seconds( 3 );
		++attempts;
		const auto function = find_create_move();
		if ( !function
			|| !state().create_move.install( reinterpret_cast<void*>( function ),
				reinterpret_cast<void*>( &create_move_hook ) ) )
		{
			if ( attempts == k_max_attempts )
				app::context().diagnostics.warning(
					"[rage] create move was not found; silent aim and anti-aim stay on the mouse." );
			return;
		}
		state().create_move_target = function;
		app::context().diagnostics.info( "[rage] command hook installed." );
		install_handle_view();
	}

	[[nodiscard]] bool command_hook_ready()
	{
		return state().create_move.active() && usercmd::ready();
	}

	void set_command( bool replace, float pitch, float yaw )
	{
		auto& wish = state().command;
		wish.pitch.store( pitch, std::memory_order_relaxed );
		wish.yaw.store( yaw, std::memory_order_relaxed );
		wish.replace.store( replace, std::memory_order_relaxed );
	}

	void set_shot( bool valid, const foundation::vec3& angles = {} )
	{
		auto& wish = state().command;
		if ( valid )
		{
			wish.shot_pitch.store( angles.x, std::memory_order_relaxed );
			wish.shot_yaw.store( angles.y, std::memory_order_relaxed );
		}
		else
			wish.fire.store( false, std::memory_order_relaxed );
		wish.shot.store( valid, std::memory_order_relaxed );
	}

	void set_view_aim( bool aim, float pitch = 0.0f, float yaw = 0.0f, float blend = 1.0f )
	{
		auto& wish = state().command;
		wish.aim_pitch.store( pitch, std::memory_order_relaxed );
		wish.aim_yaw.store( yaw, std::memory_order_relaxed );
		wish.aim_blend.store( blend, std::memory_order_relaxed );
		wish.aim.store( aim, std::memory_order_relaxed );
	}

	[[nodiscard]] int weapon_group( std::uint16_t definition )
	{
		switch ( definition )
		{
		case 1: return 2;
		case 64: return 3;
		case 2: case 3: case 4: case 30: case 32: case 36: case 61: case 63: return 1;
		case 17: case 19: case 23: case 24: case 26: case 33: case 34: return 4;
		case 25: case 27: case 29: case 35: return 6;
		case 14: case 28: return 7;
		case 40: return 8;
		case 9: return 9;
		case 11: case 38: return 10;
		case 31: return 11;
		case 7: case 8: case 10: case 13: case 16: case 39: case 60: return 5;
		default: return 0;
		}
	}

	[[nodiscard]] bool foot_bone( int bone )
	{
		return bone == game::rules::foot_heel_l || bone == game::rules::foot_heel_r
			|| bone == game::rules::foot_toes_l_t || bone == game::rules::foot_toes_r_t
			|| bone == game::rules::foot_toes_l_ct || bone == game::rules::foot_toes_r_ct;
	}

	[[nodiscard]] bool hitbox_selected( int index, int hitgroup, int bone, const bool parts[ 6 ] )
	{
		if ( index == 0 || hitgroup == 1 )
			return parts[ 0 ];
		if ( foot_bone( bone ) )
			return parts[ 5 ];
		if ( hitgroup == 2 )
			return parts[ 1 ];
		if ( hitgroup == 3 )
			return parts[ 2 ];
		if ( hitgroup == 4 || hitgroup == 5 )
			return parts[ 3 ];
		if ( hitgroup == 6 || hitgroup == 7 )
			return parts[ 4 ];
		return parts[ 1 ] || parts[ 2 ];
	}

	// The part a hit group belongs to, as estimate_hit_probability groups them.
	[[nodiscard]] int aimed_part( int hitgroup )
	{
		using part = config::combat_profile::aim_part;
		switch ( hitgroup )
		{
		case 1: return part::head;
		case 4: case 5: return part::arms;
		case 6: case 7: return part::legs;
		default: return part::body;
		}
	}

	[[nodiscard]] foundation::vec3 eye_position( std::uintptr_t pawn )
	{
		auto origin = foundation::vec3{};
		const auto node = app::context().process.load<std::uintptr_t>( pawn
			+ SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_id ) );
		if ( node )
			origin = app::context().process.load<foundation::vec3>( node
				+ SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_id ) );
		auto view = app::context().process.load<foundation::vec3>( pawn
			+ SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_id ) );
		if ( !finite_vec( view ) || view.z < 8.0f || view.z > 90.0f )
			view = { 0.0f, 0.0f, 64.0f };
		return origin + view;
	}

	[[nodiscard]] foundation::vec3 aim_punch( std::uintptr_t pawn )
	{
		const auto punch = simulation::read_cached_punch( app::context().process, pawn,
			SCHEMA( "C_CSPlayerPawn", "m_pAimPunchServices"_id ) );
		return punch ? *punch : foundation::vec3{};
	}

	[[nodiscard]] foundation::vec3 local_velocity( std::uintptr_t pawn )
	{
		auto velocity = app::context().process.load<foundation::vec3>( pawn
			+ SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_id ) );
		return finite_vec( velocity ) ? velocity : foundation::vec3{};
	}

	[[nodiscard]] bool on_ground( std::uintptr_t pawn )
	{
		const auto flags = app::context().process.load<std::uint32_t>( pawn
			+ SCHEMA( "C_BaseEntity", "m_fFlags"_id ) );
		return ( flags & 1u ) != 0u;
	}

	[[nodiscard]] bool use_held( std::uintptr_t pawn )
	{
		const auto services = app::context().process.load<std::uintptr_t>( pawn
			+ SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_id ) );
		const auto offset = SCHEMA( "CPlayer_MovementServices", "m_nButtons"_id );
		if ( !services || offset <= 0 )
			return false;
		// m_nButtons is a CInButtonState: a vtable, then m_pButtonStates[3]. The
		// vtable used to be read as the mask, and its bit 5 made "Disable On Use"
		// hold permanently.
		const auto buttons = app::context().process.load<std::uint64_t>( services + offset + 0x08 );
		return ( buttons & usercmd::in_use ) != 0u;
	}

	[[nodiscard]] bool frozen( std::uintptr_t pawn )
	{
		const auto offset = SCHEMA( "C_CSPlayerPawn", "m_bWaitForNoAttack"_id );
		if ( offset <= 0 )
			return false;
		return app::context().process.load<bool>( pawn + offset );
	}

	[[nodiscard]] bool on_ladder( std::uintptr_t pawn )
	{
		const auto offset = SCHEMA( "C_BaseEntity", "m_nActualMoveType"_id );
		if ( offset <= 0 )
			return false;
		const auto move_type = app::context().process.load<std::uint8_t>( pawn + offset );
		return move_type == 8 || move_type == 9;
	}

	[[nodiscard]] float degrees_per_pixel( std::uintptr_t pawn )
	{
		auto sensitivity = 1.0f;
		auto yaw = 0.022f;
		( void )game::variables().try_get( CONVAR( "sensitivity"_id ), sensitivity );
		( void )game::variables().try_get( CONVAR( "m_yaw"_id ), yaw );
		auto adjust = app::context().process.load<float>( pawn
			+ SCHEMA( "C_BasePlayerPawn", "m_flFOVSensitivityAdjust"_id ) );
		if ( !( adjust > 0.05f && adjust < 8.0f ) )
			adjust = 1.0f;
		if ( !( sensitivity > 0.01f ) )
			sensitivity = 1.0f;
		if ( !( yaw > 0.0001f ) )
			yaw = 0.022f;
		return sensitivity * yaw * adjust;
	}

	[[nodiscard]] float needed_damage( float minimum, int health )
	{
		if ( minimum >= 100.0f )
			return std::max( 1.0f, static_cast<float>( health ) + ( minimum - 100.0f ) );
		return std::max( minimum, 1.0f );
	}

	[[nodiscard]] int wall_count( const foundation::vec3& from, const foundation::vec3& to )
	{
		if ( !game::collision().valid() )
			return 0;
		const auto hits = game::collision().trace_ray_all( from, to );
		auto enters = 0;
		for ( const auto& hit : hits )
		{
			if ( hit.is_enter && hit.fraction < 0.98f )
				++enters;
		}
		return enters;
	}

	// Points on the capsule surface, in view space, scaled inside the spread cone.
	// A center that the cone already covers is left alone; the rest of the part is
	// only sampled where a shot can still land.
	[[nodiscard]] std::vector<foundation::vec3> surface_points(
		const game::hitbox_catalog::entry& hitbox, const foundation::vec3& center,
		const foundation::rotation& bone_rot, float scale, const foundation::vec3& eye )
	{
		std::vector<foundation::vec3> out{};
		if ( scale <= 0.01f )
			return out;

		const auto mid = ( hitbox.mins + hitbox.maxs ) * 0.5f;
		const auto capsule_a = center + bone_rot.apply( hitbox.mins - mid );
		const auto capsule_b = center + bone_rot.apply( hitbox.maxs - mid );
		const auto shoot_dir = ( center - eye ).normalized( );
		foundation::vec3 right{};
		foundation::vec3 up{};
		foundation::direction_to_angles( shoot_dir ).to_directions( nullptr, &right, &up );

		const auto surface = [ & ]( const foundation::vec3& direction ) -> foundation::vec3
		{
			const auto dir = direction.normalized( );
			if ( hitbox.radius > 0.001f )
			{
				const auto reach = ( capsule_b - capsule_a ).length( ) + hitbox.radius * 2.0f + 1.0f;
				const auto origin = center + dir * reach;
				const auto delta = dir * ( reach * -2.0f );
				float fraction = 1.0f;
				if ( foundation::ray_capsule_entry( origin, delta, capsule_a, capsule_b, hitbox.radius, fraction ) )
					return origin + delta * fraction;
			}
			else
			{
				foundation::rotation inverse{ -bone_rot.x, -bone_rot.y, -bone_rot.z, bone_rot.w };
				const auto local_dir = inverse.apply( dir );
				const auto extents = ( hitbox.maxs - hitbox.mins ) * 0.5f;
				auto distance = 8192.0f;
				if ( std::fabs( local_dir.x ) > 1.0e-6f )
					distance = std::min( distance, std::fabs( extents.x / local_dir.x ) );
				if ( std::fabs( local_dir.y ) > 1.0e-6f )
					distance = std::min( distance, std::fabs( extents.y / local_dir.y ) );
				if ( std::fabs( local_dir.z ) > 1.0e-6f )
					distance = std::min( distance, std::fabs( extents.z / local_dir.z ) );
				if ( distance < 8192.0f )
					return center + dir * distance;
			}
			return center;
		};
		const auto scaled = [ & ]( const foundation::vec3& direction )
		{
			const auto point = surface( direction );
			return center + ( point - center ) * scale;
		};

		switch ( hitbox.index )
		{
		case 0:
			out = { scaled( right ), scaled( -right ), scaled( up ), scaled( -up ) };
			break;
		case 2:
		case 3:
			out = { scaled( right ), scaled( -right ) };
			break;
		case 4:
		case 5:
		case 6:
			out = { scaled( right ), scaled( -right ) };
			if ( hitbox.index == 6 )
				out.push_back( scaled( up ) );
			break;
		case 7: case 8: case 9: case 10: case 11: case 12:
			out = { capsule_a, capsule_b };
			break;
		case 13: case 14: case 15: case 16: case 17: case 18:
			out = { capsule_b };
			break;
		default:
			out = { scaled( right ), scaled( -right ) };
			break;
		}
		return out;
	}

	// Where the eye will be once autostop has killed the horizontal speed, so the
	// aim point matches the tick the shot is actually allowed to leave.
	[[nodiscard]] foundation::vec3 stopped_eye( const foundation::vec3& eye, foundation::vec3 velocity )
	{
		velocity.z = 0.0f;
		float friction = 5.2f;
		float stop_speed = 80.0f;
		float accelerate = 5.5f;
		( void )game::variables( ).try_get( CONVAR( "sv_friction"_id ), friction );
		( void )game::variables( ).try_get( CONVAR( "sv_stopspeed"_id ), stop_speed );
		( void )game::variables( ).try_get( CONVAR( "sv_accelerate"_id ), accelerate );
		const auto tick = std::max( game::rules::simulation_step, 1.0f / 128.0f );
		const auto original = velocity;
		float elapsed = 0.0f;
		for ( int step = 0; step < 16 && velocity.length_2d( ) > 1.0f; ++step )
		{
			const auto speed = velocity.length_2d( );
			const auto drop = friction * std::max( speed, stop_speed ) * tick;
			auto next = std::max( speed - drop, 0.0f );
			const auto accel = std::min( accelerate * 250.0f * tick, next );
			next = std::max( next - accel, 0.0f );
			if ( next > 0.0f )
				velocity *= next / speed;
			else
				velocity = {};
			elapsed += tick;
		}
		const auto average = ( original + velocity ) * 0.5f;
		return { eye.x + average.x * elapsed, eye.y + average.y * elapsed, eye.z };
	}

	[[nodiscard]] std::optional<candidate> select_target(
		const config::combat_profile::rage_settings& cfg,
		const config::combat_profile::rage_weapon& weapon,
		const foundation::vec3& eye, const foundation::vec3& view,
		const std::vector<game::player_snapshot>& players, float fov_limit, float lead_seconds,
		float required_chance )
	{
		const auto any_part = std::any_of( std::begin( weapon.hitboxes ), std::end( weapon.hitboxes ),
			[]( const bool selected ) { return selected; } );
		bool parts[ 6 ]{};
		if ( any_part )
			std::copy( std::begin( weapon.hitboxes ), std::end( weapon.hitboxes ), std::begin( parts ) );
		else
			parts[ 0 ] = parts[ 1 ] = parts[ 2 ] = true;

		const auto max_walls = cfg.limiters ? std::clamp( cfg.max_walls, 0.0f, 4.0f ) : 4.0f;
		const auto per_player = cfg.double_precision ? 20 : 12;
		const auto& shot = simulation::ballistics( ).ctx( );
		const auto cone = std::max( shot.inaccuracy + shot.spread, 0.0f );
		const auto zeus = simulation::ballistics().ctx().item_def_idx == 31;
		std::optional<candidate> best{};
		auto best_score = -1.0e9f;

		struct aim_point
		{
			foundation::vec3 position{};
			int hitgroup{};
			int hitbox{ -1 };
			int priority{};
			bool center{};
		};
		// Torso centres are the widest targets and survive spread best, so they are
		// probed first; the head follows, limbs and edge points only after that.
		const auto priority_of = []( int hitgroup )
		{
			if ( hitgroup == 2 || hitgroup == 3 ) return 0;
			if ( hitgroup == 1 ) return 1;
			return 2;
		};

		for ( const auto& player : players )
		{
			if ( !game::local_player().is_enemy( player.team ) || player.health <= 0
				|| player.invulnerable || !player.pawn || !player.bones.is_valid() )
				continue;
			if ( player.pawn == game::local_player().pawn() )
				continue;

			const auto lead = player.velocity * lead_seconds;
			std::vector<aim_point> points{};
			points.reserve( 40 );
			for ( const auto& hitbox : player.hitboxes )
			{
				if ( hitbox.index < 0 || hitbox.bone < 0 || hitbox.bone >= 128 )
					continue;
				const auto hitgroup = game::hitbox_data().hitgroup_from_hitbox( hitbox.index );
				if ( !hitbox_selected( hitbox.index, hitgroup, hitbox.bone, parts ) )
					continue;
				const auto& bone = player.bones.bones[ static_cast<std::size_t>( hitbox.bone ) ];
				const auto start = bone.position + bone.rotation.apply( hitbox.mins );
				const auto end = bone.position + bone.rotation.apply( hitbox.maxs );
				const auto mid = ( start + end ) * 0.5f + lead;
				const auto priority = priority_of( hitgroup );
				if ( finite_vec( mid ) )
					points.push_back( { mid, hitgroup, hitbox.index, priority, true } );

				auto scale = cfg.double_precision ? 0.8f : 0.7f;
				if ( hitbox.radius > 0.001f )
				{
					const auto cone_radius = std::tan( cone ) * std::max( ( mid - eye ).length( ), 1.0f );
					scale = std::min( scale, std::clamp( 0.9f - cone_radius / hitbox.radius, 0.0f, 1.0f ) );
				}
				for ( const auto& edge : surface_points( hitbox, mid, bone.rotation, scale, eye ) )
				{
					if ( finite_vec( edge ) )
						points.push_back( { edge, hitgroup, hitbox.index, priority + 3, false } );
				}
			}
			std::stable_sort( points.begin(), points.end(),
				[]( const aim_point& a, const aim_point& b ) { return a.priority < b.priority; } );

			const auto needed = needed_damage( weapon.min_damage, player.health );
			std::array<bool, 20> center_done{};
			auto checked = 0;
			for ( const auto& point : points )
			{
				if ( checked >= per_player )
					break;
				if ( !point.center && point.hitbox >= 0 && point.hitbox < static_cast<int>( center_done.size( ) )
					&& center_done[ point.hitbox ] )
					continue;
				if ( zeus && eye.distance( point.position ) > k_zeus_range )
					continue;
				const auto fov = foundation::angular_distance( view, eye, point.position );
				if ( fov > fov_limit )
					continue;

				auto blocked = false;
				if ( game::collision().valid() )
				{
					const auto sight = game::collision().trace_ray( eye, point.position );
					blocked = sight.hit && sight.fraction < 0.97f;
				}
				if ( blocked && ( max_walls <= 0.0f
					|| ( max_walls < 4.0f && wall_count( eye, point.position ) > static_cast<int>( max_walls ) ) ) )
					continue;

				++checked;
				simulation::ballistics_t::penetration::result damage{};
				if ( !simulation::ballistics().pen().run( eye, point.position, player, player.bones, damage ) )
					continue;
				if ( damage.damage + 0.5f < needed )
					continue;
				// The simulated bullet has to end in a part the user allowed; a shot
				// meant for the chest that clips an arm first is not taken.
				const auto landed = damage.hitbox >= 0
					? game::hitbox_data().hitgroup_from_hitbox( damage.hitbox ) : point.hitgroup;
				if ( damage.hitbox >= 0 && !hitbox_selected( damage.hitbox, landed, -1, parts ) )
					continue;

				// The chance to land in the part this point belongs to. A point the
				// weapon can hit right now beats any it cannot, so a body shot goes
				// out instead of the bot waiting on a head it keeps missing.
				const auto angles = foundation::look_at_angles( eye, point.position );
				const auto chance = simulation::ballistics().estimate_hit_probability(
					eye, angles, player, player.bones, aimed_part( landed ) );
				const auto accurate = required_chance <= 0.0f || chance * 100.0f + 0.01f >= required_chance;

				const auto lethal = damage.damage + 0.5f >= static_cast<float>( player.health );
				if ( point.center && point.hitbox >= 0 && point.hitbox < static_cast<int>( center_done.size( ) ) )
					center_done[ point.hitbox ] = !damage.penetrated || lethal;
				auto score = damage.damage - fov * 0.5f + chance * 20.0f;
				if ( point.hitgroup == 1 )
					score += 35.0f;
				if ( accurate ) score += 200.0f;
				if ( lethal ) score += 100.0f;
				if ( point.center ) score += 10.0f;
				if ( !blocked ) score += 5.0f;
				if ( score > best_score )
				{
					best_score = score;
					candidate next{};
					next.player = &player;
					next.point = point.position;
					next.angles = angles;
					next.hitgroup = landed;
					next.damage = damage.damage;
					next.fov = fov;
					next.chance = chance;
					next.penetrated = damage.penetrated || blocked;
					best = next;
				}
				if ( lethal && accurate && point.center && !blocked )
					break;
			}
		}
		return best;
	}

	[[nodiscard]] game::input_binding binding_for( game::input_action action )
	{
		return game::input_bindings().resolve( action );
	}

	[[nodiscard]] std::uint16_t keyboard_key( const game::input_binding& binding )
	{
		if ( binding.device != game::input_device::keyboard || !binding.virtual_key )
			return 0;
		return binding.virtual_key;
	}

	void set_key( std::uint16_t key, bool down, bool& held )
	{
		if ( !key || held == down )
			return;
		if ( app::context().input.key( key, down ) )
			held = down;
	}

	void set_owned_movement( const std::vector<std::uint16_t>& wanted )
	{
		auto& owned = state().owned_move;
		std::vector<platform::windows::input_gateway::key_transition> changes{};
		for ( const auto key : owned )
		{
			if ( std::ranges::find( wanted, key ) == wanted.end() )
				changes.push_back( { key, false } );
		}
		for ( const auto key : wanted )
		{
			if ( std::ranges::find( owned, key ) == owned.end() )
				changes.push_back( { key, true } );
		}
		if ( !changes.empty() && !app::context().input.keys( changes ) )
			return;
		owned = wanted;
	}

	void release_owned_movement()
	{
		set_owned_movement( {} );
	}

	void counter_strafe( std::uintptr_t pawn, const foundation::vec3& view )
	{
		const auto velocity = local_velocity( pawn );
		foundation::vec3 forward{};
		foundation::vec3 right{};
		view.to_directions( &forward, &right, nullptr );
		forward.z = 0.0f;
		right.z = 0.0f;
		if ( forward.length_sqr() < 1.0e-4f || right.length_sqr() < 1.0e-4f )
		{
			release_owned_movement();
			return;
		}
		forward.normalize();
		right.normalize();
		const auto along = velocity.dot( forward );
		const auto side = velocity.dot( right );
		std::vector<std::uint16_t> keys{};
		const auto add = [ & ]( game::input_action action )
		{
			const auto key = keyboard_key( binding_for( action ) );
			if ( key )
				keys.push_back( key );
		};
		if ( along > 12.0f ) add( game::input_action::back );
		if ( along < -12.0f ) add( game::input_action::forward );
		if ( side > 12.0f ) add( game::input_action::left );
		if ( side < -12.0f ) add( game::input_action::right );
		set_owned_movement( keys );
	}

	void move_toward( const foundation::vec3& from, const foundation::vec3& to, const foundation::vec3& view )
	{
		auto delta = to - from;
		delta.z = 0.0f;
		if ( delta.length_sqr() < 12.0f * 12.0f )
		{
			release_owned_movement();
			return;
		}
		foundation::vec3 forward{};
		foundation::vec3 right{};
		view.to_directions( &forward, &right, nullptr );
		forward.z = 0.0f;
		right.z = 0.0f;
		forward.normalize();
		right.normalize();
		const auto wish_forward = delta.dot( forward );
		const auto wish_side = delta.dot( right );
		std::vector<std::uint16_t> keys{};
		const auto add = [ & ]( game::input_action action )
		{
			const auto key = keyboard_key( binding_for( action ) );
			if ( key )
				keys.push_back( key );
		};
		if ( wish_forward > 8.0f ) add( game::input_action::forward );
		if ( wish_forward < -8.0f ) add( game::input_action::back );
		if ( wish_side > 8.0f ) add( game::input_action::right );
		if ( wish_side < -8.0f ) add( game::input_action::left );
		set_owned_movement( keys );
	}

	void tap_scope( bool down )
	{
		auto& runtime = state();
		if ( runtime.scope_down == down )
			return;
		const auto binding = binding_for( game::input_action::attack2 );
		auto sent = false;
		if ( binding.device == game::input_device::mouse_secondary || binding.device == game::input_device::none )
		{
			sent = app::context().input.pointer( 0, 0, down
				? platform::windows::pointer_action::secondary_down
				: platform::windows::pointer_action::secondary_up );
		}
		else if ( const auto key = keyboard_key( binding ) )
			sent = app::context().input.key( key, down );
		if ( sent )
			runtime.scope_down = down;
	}

	void set_attack( bool down )
	{
		auto& runtime = state();
		if ( runtime.attack_down == down )
			return;
		if ( app::context().input.pointer( 0, 0, down
			? platform::windows::pointer_action::primary_down
			: platform::windows::pointer_action::primary_up ) )
		{
			runtime.attack_down = down;
			if ( down )
				runtime.attack_down_at = std::chrono::steady_clock::now();
		}
	}

	void hold_duck( bool down )
	{
		const auto key = keyboard_key( binding_for( game::input_action::duck ) );
		set_key( key, down, state().duck_down );
	}

	void apply_fps_spoof( bool enabled )
	{
		auto& runtime = state();
		if ( enabled == runtime.fps_spoofed )
			return;
		const auto cvar = CONVAR( "fps_max"_id );
		if ( !cvar )
			return;
		if ( enabled )
		{
			float current = 0.0f;
			if ( game::variables().try_get<float>( cvar, current ) )
				runtime.saved_fps = current;
			runtime.fps_spoofed = game::variables().set<float>( cvar, 0.0f );
			return;
		}
		if ( runtime.saved_fps >= 0.0f )
			( void )game::variables().set<float>( cvar, runtime.saved_fps );
		runtime.fps_spoofed = false;
		runtime.saved_fps = -1.0f;
	}

	void publish_marker( bool draw, bool returning, const foundation::vec3& origin, const float color[ 4 ] )
	{
		std::scoped_lock lock( state().marker_lock );
		auto& marker = state().peek_marker;
		marker.draw = draw;
		marker.returning = returning;
		marker.origin = origin;
		std::copy( color, color + 4, std::begin( marker.color ) );
	}

	[[nodiscard]] foundation::vec3 anti_aim_angles(
		const config::combat_profile::rage_settings& cfg,
		const foundation::vec3& view, const foundation::vec3& eye,
		const game::player_snapshot* target, runtime& runtime )
	{
		auto yaw_base = view.y;
		if ( cfg.yaw_base == 1 && target )
			yaw_base = foundation::look_at_angles( eye, target->origin ).y;

		const auto degree = std::clamp( cfg.yaw_modifier_degree, 0.0f, 180.0f );
		runtime.spin_clock += game::rules::simulation_step;
		auto yaw = yaw_base + cfg.yaw_offset;
		switch ( cfg.yaw )
		{
		case 1: break;
		case 2: yaw += runtime.spin_clock * std::max( degree, 30.0f ); break;
		case 3: yaw += ( static_cast<int>( runtime.spin_clock * 64.0f ) & 1 ) ? degree : -degree; break;
		default: yaw += 180.0f; break;
		}
		switch ( cfg.yaw_modifier )
		{
		case 1: yaw += ( static_cast<int>( runtime.spin_clock * 32.0f ) & 1 ) ? degree * 0.5f : -degree * 0.5f; break;
		case 2: yaw += degree; break;
		case 3: yaw += ( next_unit( runtime ) * 2.0f - 1.0f ) * degree; break;
		case 4: yaw += runtime.spin_clock * degree; break;
		default: break;
		}

		if ( cfg.mouse_override )
		{
			const auto moved = runtime.view_yaw_valid
				&& std::abs( foundation::wrap_yaw( view.y - runtime.last_view_yaw ) ) > 0.15f;
			const auto blend = std::clamp( cfg.mouse_override_speed / 100.0f, 0.0f, 1.0f );
			if ( moved || !cfg.mouse_override_return )
				yaw = foundation::wrap_yaw( yaw + foundation::wrap_yaw( view.y - yaw ) * blend );
		}
		runtime.last_view_yaw = view.y;
		runtime.view_yaw_valid = true;

		auto pitch = view.x;
		switch ( cfg.pitch )
		{
		case 1: pitch = 89.0f; break;
		case 2: pitch = -89.0f; break;
		case 3: pitch = 0.0f; break;
		case 4:
		{
			const auto span = std::clamp( std::abs( cfg.pitch_offset ), 0.0f, 89.0f );
			pitch = ( next_unit( runtime ) * 2.0f - 1.0f ) * ( span > 0.0f ? span : 89.0f );
			break;
		}
		default: break;
		}
		return { std::clamp( pitch, -89.0f, 89.0f ), foundation::wrap_yaw( yaw ), 0.0f };
	}

	void release_combat_input()
	{
		set_attack( false );
		tap_scope( false );
		hold_duck( false );
		release_owned_movement();
		state().command.stop.store( false, std::memory_order_relaxed );
		set_command( false, 0.0f, 0.0f );
		set_shot( false );
		set_view_aim( false );
		state().returning = false;
		state().second_shot = false;
		state().queued_second = false;
		state().shot_clip = -1;
		state().shot_until = {};
		state().shot_hold_until = {};
		state().fire_pending = false;
	}

} // namespace

void ragebot_t::reset()
{
	auto& runtime = state();
	if ( !runtime.was_enabled && !runtime.attack_down && !runtime.scope_down
		&& !runtime.duck_down && runtime.owned_move.empty() && !runtime.peek_saved )
		return;
	release_combat_input();
	apply_fps_spoof( false );
	publish_marker( false, false, {}, runtime.peek_marker.color );
	runtime.was_enabled = false;
	runtime.peek_saved = false;
	runtime.tracked_pawn = 0;
	runtime.view_yaw_valid = false;
}

namespace {
	struct preview_slot
	{
		std::mutex lock{};
		aim_preview value{};
		std::chrono::steady_clock::time_point stamp{};
		bool valid{};
	};

	preview_slot& preview()
	{
		static preview_slot value{};
		return value;
	}
} // namespace

void publish_aim_preview( const aim_preview& value )
{
	auto& slot = preview();
	std::scoped_lock lock( slot.lock );
	slot.value = value;
	slot.stamp = std::chrono::steady_clock::now();
	slot.valid = value.pawn != 0;
}

void clear_aim_preview()
{
	auto& slot = preview();
	std::scoped_lock lock( slot.lock );
	slot.valid = false;
}

std::optional<aim_preview> latest_aim_preview()
{
	auto& slot = preview();
	std::scoped_lock lock( slot.lock );
	if ( !slot.valid || std::chrono::steady_clock::now() - slot.stamp > std::chrono::milliseconds( 150 ) )
		return std::nullopt;
	return slot.value;
}

void ensure_command_hook()
{
	static std::mutex lock{};
	std::scoped_lock guard( lock );
	install_create_move();
}

bool command_hook_active()
{
	return command_hook_ready();
}

void set_bhop_wish( bool armed, bool hold )
{
	g_bhop_hold.store( armed && hold, std::memory_order_relaxed );
	g_bhop_armed.store( armed, std::memory_order_relaxed );
}

void ragebot_t::tick()
{
	ensure_command_hook();
	auto& runtime = state();
	const auto snapshot = config::get_runtime_snapshot();
	const auto enabled = snapshot && snapshot->combat.rage.enabled
		&& game::local_player().alive();
	if ( !enabled )
	{
		if ( runtime.was_enabled )
			reset();
		return;
	}
	runtime.was_enabled = true;

	const auto& cfg = snapshot->combat.rage;
	apply_fps_spoof( cfg.limiters && cfg.spoof_fps );

	const auto pawn = game::local_player().pawn();
	const auto weapon_ctx = simulation::ballistics().ctx();
	const auto& weapon = cfg.weapons[ static_cast<std::size_t>(
		weapon_group( weapon_ctx.item_def_idx ) ) ];
	const auto now = std::chrono::steady_clock::now();
	auto eye = eye_position( pawn );
	const auto velocity = local_velocity( pawn );
	if ( cfg.limiters && cfg.selftrack )
		eye -= velocity * game::rules::simulation_step;
	const auto grounded_now = on_ground( pawn );
	foundation::vec3 aim_eye = eye;
	if ( weapon.quick_stop && weapon.quick_stop_options[ 2 ] && grounded_now && velocity.length_2d( ) > 15.0f )
		aim_eye = stopped_eye( eye, velocity );
	foundation::vec3 view{};
	if ( !usercmd::cached_view_angles( view ) )
		view = game::camera().angles();
	if ( !finite_vec( view ) )
		view = app::context().process.load<foundation::vec3>( pawn
			+ SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_id ) );
	const auto punch = aim_punch( pawn );
	const auto grounded = on_ground( pawn );
	const auto players = game::world().players();

	if ( cfg.quick_peek_assist && !runtime.peek_saved )
	{
		runtime.peek_origin = eye;
		runtime.peek_origin.z -= 64.0f;
		if ( game::collision().valid() )
		{
			const auto ground = game::collision().trace_ray(
				eye, eye - foundation::vec3{ 0.0f, 0.0f, 256.0f } );
			if ( ground.hit )
				runtime.peek_origin = ground.end_pos;
		}
		runtime.peek_saved = true;
		runtime.returning = false;
	}
	if ( !cfg.quick_peek_assist )
	{
		runtime.peek_saved = false;
		runtime.returning = false;
	}

	const auto firearm = game::rules::is_firearm( weapon_ctx.weapon_type )
		|| weapon_ctx.item_def_idx == 31;
	auto fov_limit = cfg.field_of_view < 1.0f ? 180.0f : cfg.field_of_view;
	if ( cfg.limiters )
		fov_limit = std::min( fov_limit, std::max( cfg.aim_fov, 1.0f ) );

	// Lag compensation rewinds the target to what this client rendered, so the
	// bones are already where the bullet lands; leading them by the backtrack
	// window threw shots tens of units ahead of a running target. At most the
	// one tick between sampling the bones and the command leaving is covered.
	const auto lead = game::rules::simulation_step * std::clamp( weapon.extrapolation, 0.0f, 100.0f ) / 100.0f;

	auto hitchance = weapon.hit_chance;
	if ( runtime.second_shot )
		hitchance = weapon.hit_chance_double_tap;
	// Remove Spread picks the accuracy the bot waits for when no hit chance is
	// set. It used to demand a near-perfect weapon and under 40 u/s, which a
	// walking or crouch-walking player never reaches: the bot held fire for
	// as long as he moved. The chance already accounts for movement.
	if ( hitchance <= 0.0f && cfg.remove_spread > 0 )
	{
		const auto sniper = weapon_ctx.weapon_type == game::rules::precision;
		hitchance = cfg.remove_spread >= 2 ? ( sniper ? 75.0f : 60.0f ) : ( sniper ? 55.0f : 40.0f );
	}

	std::optional<candidate> target{};
	if ( firearm && players && weapon_ctx.valid && !weapon_ctx.is_reloading && weapon_ctx.clip != 0 )
		target = select_target( cfg, weapon, aim_eye, view, *players, fov_limit, lead, hitchance );

	if ( runtime.tracked_pawn && players )
	{
		const game::player_snapshot* previous = nullptr;
		for ( const auto& player : *players )
		{
			if ( player.pawn == runtime.tracked_pawn )
			{
				previous = &player;
				break;
			}
		}
		if ( !previous || previous->health <= 0 )
		{
			if ( cfg.limiters && runtime.tracked_health > 0 )
				runtime.kill_until = now + std::chrono::milliseconds(
					static_cast<int>( std::clamp( cfg.kill_delay, 0.0f, 1000.0f ) ) );
			runtime.tracked_pawn = 0;
			runtime.tracked_health = 0;
		}
		else
			runtime.tracked_health = previous->health;
	}

	if ( target && target->player )
	{
		if ( target->player->pawn != runtime.tracked_pawn )
		{
			runtime.tracked_pawn = target->player->pawn;
			runtime.tracked_since = now;
			runtime.tracked_health = target->player->health;
		}
	}

	const auto kill_locked = cfg.limiters && now < runtime.kill_until;
	const auto reaction_locked = cfg.limiters && target
		&& std::chrono::duration<float, std::milli>( now - runtime.tracked_since ).count() < cfg.reaction_time;
	const auto aim_ready = target && !kill_locked && !reaction_locked;
	// The command angles get the recoil taken out; the server adds it back, so
	// the chance was sampled around the uncompensated angles.
	if ( aim_ready )
	{
		target->angles.x -= punch.x;
		target->angles.y = foundation::wrap_yaw( target->angles.y - punch.y );
		target->angles.x = std::clamp( target->angles.x, -89.0f, 89.0f );
	}

	const auto speed = local_velocity( pawn ).length_2d();
	const auto want_stop = weapon.quick_stop && aim_ready
		&& ( grounded || weapon.quick_stop_options[ 3 ] );
	const auto stop_now = want_stop && ( weapon.quick_stop_options[ 0 ]
		|| ( weapon.quick_stop_options[ 1 ] && !weapon_ctx.weapon_ready )
		|| ( weapon.quick_stop_options[ 2 ] && speed > 15.0f ) );
	const auto returning_peek = cfg.quick_peek_assist && runtime.returning;
	// With the command hook the stop is written into every command (see
	// create_move_hook); the key path is only the fallback without it.
	const auto command_stop = stop_now && !returning_peek && command_hook_ready();
	runtime.command.stop.store( command_stop, std::memory_order_relaxed );
	if ( command_stop )
		release_owned_movement();
	else if ( stop_now && !returning_peek )
		counter_strafe( pawn, view );
	else if ( !returning_peek )
		release_owned_movement();

	const auto stopped_enough = !weapon.quick_stop || !weapon.quick_stop_options[ 2 ] || speed < 15.0f;
	auto delay_shot = false;
	if ( cfg.limiters && aim_ready && target )
	{
		const auto since_acquire = std::chrono::duration<float, std::milli>( now - runtime.tracked_since ).count();
		const auto peeking = cfg.delay_shot[ 0 ] && since_acquire < cfg.delay_shot_time + 1.0f;
		const auto unlag = cfg.delay_shot[ 1 ] && lead > 0.0f;
		const auto lethal = cfg.delay_shot[ 2 ] && target->damage + 0.5f >= static_cast<float>( target->player->health );
		if ( ( peeking || unlag || lethal ) && since_acquire < cfg.delay_shot_time )
			delay_shot = true;
	}

	const auto error = aim_ready
		? foundation::angular_distance( view, eye, target->point ) : 180.0f;
	const auto refined = !cfg.refine_shot || cfg.silent_aim || error < 0.55f;
	// Only the part being aimed at counts (see select_target): a head shot that
	// would land in the chest is a miss for what was picked. Shown on the ESP.
	const auto probability = target ? target->chance : 1.0f;
	const auto strict_blocked = weapon.hit_chance_strict && target && target->penetrated;
	const auto chance_ready = hitchance <= 0.0f || probability * 100.0f + 0.01f >= hitchance;
	const auto needs_scope = weapon.quick_scope
		&& weapon_ctx.weapon_type == game::rules::precision && !weapon_ctx.is_scoped;
	if ( needs_scope && aim_ready )
		tap_scope( true );
	else
		tap_scope( false );

	const auto revolver = weapon_ctx.item_def_idx == 64;
	const auto revolver_ready = !revolver || weapon_ctx.postpone_fire_ready_tick <= 0
		|| weapon_ctx.postpone_fire_ready_tick <= weapon_ctx.player_tick;
	const auto want_fire = cfg.auto_fire && aim_ready && stopped_enough
		&& !delay_shot && refined && chance_ready && !strict_blocked && !needs_scope
		&& weapon_ctx.weapon_ready && revolver_ready;

	if ( target && target->player )
		publish_aim_preview( { target->player->pawn, target->point,
			probability * 100.0f, target->damage, want_fire } );
	else
		clear_aim_preview();

	if ( cfg.duck_peek_assist && grounded && !want_fire )
		hold_duck( true );
	else
		hold_duck( false );

	const auto silent = cfg.silent_aim && command_hook_ready();
	const auto blend = cfg.aim_speed <= 0.0f ? 1.0f : std::clamp( cfg.aim_speed / 100.0f, 0.05f, 1.0f );
	const auto view_hook = command_hook_ready();
	if ( aim_ready && !silent && view_hook )
		set_view_aim( true, target->angles.x, target->angles.y, blend );
	else
		set_view_aim( false );

	// Silent aim: every command that shoots leaves with the target angles, the
	// player's own clicks included. They are refreshed each worker tick and held
	// briefly so a click landing right after the target flickers still uses them.
	if ( silent && aim_ready )
	{
		runtime.shot_angles = target->angles;
		runtime.shot_hold_until = now + std::chrono::milliseconds( 60 );
	}
	const auto shot_valid = silent && now < runtime.shot_hold_until;
	set_shot( shot_valid, runtime.shot_angles );

	if ( aim_ready && !silent && !view_hook && cfg.aim_speed > 0.0f )
	{
		const auto scale = degrees_per_pixel( pawn );
		auto pitch_delta = target->angles.x - view.x;
		auto yaw_delta = foundation::wrap_yaw( target->angles.y - view.y );
		const auto blend = std::clamp( cfg.aim_speed / 100.0f, 0.0f, 1.0f );
		pitch_delta *= blend;
		yaw_delta *= blend;
		const auto dx = static_cast<int>( std::lround( -yaw_delta / scale ) );
		const auto dy = static_cast<int>( std::lround( pitch_delta / scale ) );
		if ( dx || dy )
			app::context().input.pointer( dx, dy, platform::windows::pointer_action::relative_move );
	}

	// With the command hook the shot is pressed inside the command itself, so it
	// leaves on the tick it was aimed for. A synthetic click used to land one or
	// more commands later, by which time the angles no longer matched. The R8
	// still needs attack held to cock, so it keeps the mouse.
	const auto command_fire = silent && !revolver;
	auto& wish = runtime.command;
	if ( command_fire )
	{
		if ( runtime.attack_down )
			set_attack( false );
		const auto fired = wish.fired.load( std::memory_order_relaxed );
		if ( fired != runtime.seen_fired )
		{
			runtime.seen_fired = fired;
			if ( runtime.fire_pending )
			{
				runtime.fire_pending = false;
				runtime.fire_block_until = now + std::chrono::milliseconds( 40 );
				runtime.second_shot = cfg.double_tap && !runtime.second_shot;
				if ( cfg.quick_peek_assist && cfg.quick_peek_retreat )
					runtime.returning = true;
			}
		}
		if ( runtime.fire_pending && ( !aim_ready
			|| now - runtime.fire_requested_at > std::chrono::milliseconds( 150 ) ) )
		{
			wish.fire.store( false, std::memory_order_relaxed );
			runtime.fire_pending = false;
		}
		if ( want_fire && !runtime.fire_pending && now >= runtime.fire_block_until )
		{
			runtime.fire_pending = true;
			runtime.fire_requested_at = now;
			wish.fire.store( true, std::memory_order_relaxed );
		}
	}
	else
	{
		if ( runtime.fire_pending )
		{
			wish.fire.store( false, std::memory_order_relaxed );
			runtime.fire_pending = false;
		}
		if ( revolver && aim_ready && !revolver_ready )
			set_attack( true );
		else if ( want_fire && !runtime.attack_down )
		{
			set_attack( true );
			runtime.shot_until = now + std::chrono::milliseconds( 48 );
			runtime.queued_second = cfg.double_tap && !runtime.second_shot;
			runtime.second_shot = false;
			if ( cfg.quick_peek_assist && cfg.quick_peek_retreat )
				runtime.returning = true;
		}
		else if ( runtime.attack_down
			&& now - runtime.attack_down_at > std::chrono::milliseconds( 16 )
			&& ( runtime.queued_second || !weapon_ctx.is_full_auto || !want_fire ) )
		{
			set_attack( false );
			runtime.second_shot = runtime.queued_second;
			runtime.queued_second = false;
		}
	}

	// The hook itself also skips commands that shoot or hold +use; these
	// checks cover the worker-side state (aim being dragged on the mouse path).
	const auto mouse_shooting = !command_fire && now < runtime.shot_until && aim_ready;
	wish.skip_on_use.store( cfg.anti_aim_on_use, std::memory_order_relaxed );
	const auto aa_allowed = cfg.anti_aim && command_hook_ready() && !mouse_shooting
		&& !( cfg.anti_aim_on_use && use_held( pawn ) )
		&& !( cfg.anti_aim_freeze_time && frozen( pawn ) )
		&& !on_ladder( pawn );
	if ( aa_allowed )
	{
		const auto angles = anti_aim_angles( cfg, view, eye, target ? target->player : nullptr, runtime );
		set_command( true, angles.x, angles.y );
	}
	else
		set_command( false, 0.0f, 0.0f );

	if ( cfg.quick_peek_assist && runtime.returning && runtime.peek_saved )
	{
		const auto feet = eye - foundation::vec3{ 0.0f, 0.0f, 64.0f };
		if ( feet.distance( runtime.peek_origin ) < 16.0f )
		{
			runtime.returning = false;
			if ( cfg.quick_peek_auto_stop )
				counter_strafe( pawn, view );
			else
				release_owned_movement();
		}
		else
			move_toward( feet, runtime.peek_origin, view );
	}

	publish_marker( cfg.quick_peek_assist && runtime.peek_saved, runtime.returning,
		runtime.peek_origin, cfg.quick_peek_color );
}

std::string ragebot_t::report() const
{
	const auto snapshot = config::get_runtime_snapshot();
	const auto& wish = state().command;
	return std::format( "enabled={} aa={} armed={} view_hook={} usercmd={} alive={} calls={} writes={} shots={} "
		"view_fixes={} failed={} replace={} shot={} target={:#x}",
		snapshot && snapshot->combat.rage.enabled, snapshot && snapshot->combat.rage.anti_aim,
		state().create_move.active(), state().handle_view.active(), usercmd::ready(), game::local_player().alive(),
		g_move_calls.load( std::memory_order_relaxed ), g_move_writes.load( std::memory_order_relaxed ),
		g_move_shots.load( std::memory_order_relaxed ), g_view_restores.load( std::memory_order_relaxed ),
		g_move_no_cmd.load( std::memory_order_relaxed ), wish.replace.load( std::memory_order_relaxed ),
		wish.shot.load( std::memory_order_relaxed ), state().create_move_target );
}

void ragebot_t::on_render( zdraw::draw_list& draw_list ) const
{
	marker copy{};
	{
		std::scoped_lock lock( state().marker_lock );
		copy = state().peek_marker;
	}
	if ( !copy.draw || !finite_vec( copy.origin ) )
		return;

	std::array<float, 64> points{};
	auto count = 0;
	constexpr auto segments = 24;
	for ( int index = 0; index <= segments; ++index )
	{
		const auto angle = static_cast<float>( index ) / static_cast<float>( segments )
			* std::numbers::pi_v<float> * 2.0f;
		const auto world = copy.origin + foundation::vec3{
			std::cos( angle ) * 18.0f, std::sin( angle ) * 18.0f, 1.0f };
		const auto screen = game::camera().project( world );
		if ( !game::camera().projection_valid( screen ) )
			return;
		points[ static_cast<std::size_t>( count++ ) ] = screen.x;
		points[ static_cast<std::size_t>( count++ ) ] = screen.y;
	}
	const auto alpha = copy.returning ? 1.0f : 0.85f;
	const zdraw::rgba color{
		static_cast<std::uint8_t>( std::clamp( copy.color[ 0 ], 0.0f, 1.0f ) * 255.0f ),
		static_cast<std::uint8_t>( std::clamp( copy.color[ 1 ], 0.0f, 1.0f ) * 255.0f ),
		static_cast<std::uint8_t>( std::clamp( copy.color[ 2 ], 0.0f, 1.0f ) * 255.0f ),
		static_cast<std::uint8_t>( std::clamp( copy.color[ 3 ] * alpha, 0.0f, 1.0f ) * 255.0f ) };
	draw_list.add_polyline( std::span<const float>{ points.data(), static_cast<std::size_t>( count ) },
		color, true, 1.6f );
}

} // namespace features::combat
