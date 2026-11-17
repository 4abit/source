#include <stdafx.hpp>

#include <features/misc/thirdperson.hpp>
#include <features/combat/usercmd.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <app/context.hpp>
#include <config/settings.hpp>
#include <core/state/services.hpp>

namespace features::misc {
namespace {

	constexpr std::uintptr_t k_origin = 0x4A0;
	constexpr float k_hull = 9.0f;

	struct wish
	{
		std::atomic<bool> enabled{ false };
		std::atomic<bool> collision{ true };
		std::atomic<float> distance{ 120.0f };
		std::atomic<std::uintptr_t> pawn{ 0 };
		std::atomic<bool> alive{ false };
	};

	wish g_wish{};

	[[nodiscard]] bool finite_vec( const foundation::vec3& value )
	{
		return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z );
	}

	[[nodiscard]] double now_seconds()
	{
		using clock = std::chrono::steady_clock;
		return std::chrono::duration<double>( clock::now().time_since_epoch() ).count();
	}

	[[nodiscard]] foundation::vec3 networked_eye( std::uintptr_t pawn )
	{
		const auto& process = app::context().process;
		const auto node = process.load<std::uintptr_t>( pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_id ) );
		auto origin = node ? process.load<foundation::vec3>( node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_id ) )
			: foundation::vec3{};
		auto offset = process.load<foundation::vec3>( pawn + SCHEMA( "C_BaseModelEntity", "m_vecViewOffset"_id ) );
		if ( !finite_vec( offset ) || offset.z < 8.0f || offset.z > 90.0f )
			offset = { 0.0f, 0.0f, 64.0f };
		return origin + offset;
	}

	[[nodiscard]] bool view_angles( std::uintptr_t pawn, foundation::vec3& out )
	{
		if ( combat::usercmd::resolve() && combat::usercmd::view_angles( out ) && finite_vec( out ) )
			return true;
		out = app::context().process.load<foundation::vec3>( pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_id ) );
		return finite_vec( out );
	}

	[[nodiscard]] std::uintptr_t observer_services()
	{
		const auto controller = game::local_player().controller();
		if ( !controller )
			return 0;
		const auto& process = app::context().process;
		const auto handle = process.load<std::uint32_t>( controller + SCHEMA( "CCSPlayerController", "m_hObserverPawn"_id ) );
		if ( !handle || handle == 0xffffffffu )
			return 0;
		const auto pawn = game::entity_index().lookup( handle );
		return pawn ? process.load<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pObserverServices"_id ) ) : 0;
	}

} // namespace

void thirdperson_t::on_view( std::uintptr_t view_setup )
{
	const auto now = now_seconds();
	const auto dt = static_cast<float>( std::clamp( now - m_last_frame, 0.0, 0.1 ) );
	m_last_frame = now;

	const auto pawn = g_wish.pawn.load( std::memory_order_relaxed );
	const auto active = g_wish.enabled.load( std::memory_order_relaxed )
		&& g_wish.alive.load( std::memory_order_relaxed ) && pawn;

	// Eased in and out so a bind press slides the camera instead of cutting.
	const auto target = active ? 1.0f : 0.0f;
	m_boom += ( target - m_boom ) * std::min( 1.0f, dt * 9.0f );
	if ( std::abs( target - m_boom ) < 0.002f )
		m_boom = target;
	if ( m_boom <= 0.0f || !pawn )
	{
		m_reach = -1.0f;
		return;
	}

	auto* origin = reinterpret_cast<foundation::vec3*>( view_setup + k_origin );
	const auto fallback = networked_eye( pawn );
	// The engine just wrote the interpolated eye here; the networked origin
	// only steps once per tick and is kept as a sanity check.
	const auto eye = finite_vec( *origin ) && origin->distance( fallback ) < 96.0f ? *origin : fallback;

	foundation::vec3 angles{};
	if ( !view_angles( pawn, angles ) )
		return;
	foundation::vec3 forward{};
	angles.to_directions( &forward, nullptr, nullptr );
	if ( forward.length_sqr() < 1.0e-6f )
		return;

	const auto distance = std::clamp( g_wish.distance.load( std::memory_order_relaxed ), 30.0f, 300.0f ) * m_boom;
	auto reach = distance;
	if ( g_wish.collision.load( std::memory_order_relaxed ) && game::collision().valid() && distance > 1.0f )
	{
		const foundation::vec3 half{ k_hull, k_hull, k_hull };
		const auto trace = game::collision().sweep_hull( eye, eye - forward * distance, half );
		if ( trace.hit && trace.fraction < 1.0f )
			reach = distance * std::clamp( trace.fraction, 0.0f, 1.0f );
	}

	// Walls pull the camera in at once so it never clips; open space lets it
	// glide back out.
	if ( m_reach < 0.0f || reach < m_reach )
		m_reach = reach;
	else
		m_reach += ( reach - m_reach ) * std::min( 1.0f, dt * 7.0f );

	*origin = eye - forward * m_reach;
}

void thirdperson_t::tick()
{
	const auto snapshot = config::get_runtime_snapshot();
	const auto enabled = snapshot && snapshot->visual.m_thirdperson.enabled;
	const auto while_dead = snapshot && snapshot->visual.m_thirdperson.while_dead;
	const auto distance = snapshot ? snapshot->visual.m_thirdperson.distance : 120.0f;
	const auto alive = game::local_player().alive();

	g_wish.enabled.store( enabled, std::memory_order_relaxed );
	g_wish.collision.store( snapshot && snapshot->visual.m_thirdperson.collision, std::memory_order_relaxed );
	g_wish.distance.store( distance, std::memory_order_relaxed );
	g_wish.alive.store( alive, std::memory_order_relaxed );
	g_wish.pawn.store( game::local_player().pawn(), std::memory_order_relaxed );

	auto& process = app::context().process;
	const auto mode_offset = SCHEMA( "CPlayer_ObserverServices", "m_iObserverMode"_id );
	const auto distance_offset = SCHEMA( "CPlayer_ObserverServices", "m_flObserverChaseDistance"_id );
	if ( enabled && while_dead && !alive )
	{
		const auto services = observer_services();
		if ( !services || mode_offset <= 0 )
			return;
		if ( !m_observer_forced )
		{
			m_saved_observer_mode = process.load<std::uint8_t>( services + mode_offset );
			m_observer_forced = true;
		}
		constexpr std::uint8_t chase = 3;
		if ( process.load<std::uint8_t>( services + mode_offset ) != chase )
			( void )process.store<std::uint8_t>( services + mode_offset, chase );
		if ( distance_offset > 0 )
			( void )process.store<float>( services + distance_offset, std::clamp( distance, 30.0f, 300.0f ) );
		return;
	}

	if ( !m_observer_forced )
		return;
	if ( const auto services = observer_services(); services && mode_offset > 0 && !alive )
		( void )process.store<std::uint8_t>( services + mode_offset, m_saved_observer_mode );
	m_observer_forced = false;
}

} // namespace features::misc
