#include <stdafx.hpp>

#include <features/combat/usercmd.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace features::combat::usercmd {
namespace {

	// Raw protobuf messages carry a vtable and an arena in front of the fields.
	constexpr std::uintptr_t k_impl = 0x10;

	// CUserCmd: the CSGOUserCmdPB body starts at +0x20.
	constexpr std::uintptr_t k_cmd_history = 0x28;   // RepeatedPtrField<CSGOInputHistoryEntryPB>
	constexpr std::uintptr_t k_cmd_base = 0x40;      // CBaseUserCmdPB*
	constexpr std::uintptr_t k_cmd_attack1 = 0x4c;   // attack1_start_history_index
	// CInButtonState right after the protobuf: vtable, then held / changed / scroll.
	// The client serialises the protobuf button state from it.
	constexpr std::uintptr_t k_cmd_buttons = 0x58;

	// CBaseUserCmdPB body.
	constexpr std::uintptr_t k_base_subticks = 0x08;
	constexpr std::uintptr_t k_base_crc = 0x20;
	constexpr std::uintptr_t k_base_buttons = 0x28;
	constexpr std::uintptr_t k_base_angles = 0x30;
	constexpr std::uintptr_t k_base_forward = 0x48;
	constexpr std::uintptr_t k_base_left = 0x4c;

	// CMsgQAngle body.
	constexpr std::uintptr_t k_angle_x = 0x08;
	constexpr std::uintptr_t k_angle_y = 0x0c;
	constexpr std::uintptr_t k_angle_z = 0x10;

	// CSGOInputHistoryEntryPB body.
	constexpr std::uintptr_t k_history_angles = 0x08;

	// CSubtickMoveStep body.
	constexpr std::uintptr_t k_step_button = 0x08;
	constexpr std::uintptr_t k_step_pressed = 0x10;
	constexpr std::uintptr_t k_step_forward = 0x18;
	constexpr std::uintptr_t k_step_left = 0x1c;

	// CInButtonStatePB body.
	constexpr std::uintptr_t k_buttons_state = 0x08;

	using get_base_fn = std::uintptr_t( __fastcall* )( std::uintptr_t );
	using get_cmd_fn = std::uintptr_t( __fastcall* )( std::uintptr_t, int );
	using get_angles_fn = float*( __fastcall* )( std::uintptr_t, int );
	using set_angles_fn = void( __fastcall* )( std::uintptr_t, int, const float* );
	using string_copy_fn = void( __fastcall* )( void*, const void*, int );
	using serialize_crc_fn = void( __fastcall* )( void*, void*, std::uintptr_t );
	using step_alloc_fn = void*( __fastcall* )( std::uintptr_t );
	using field_push_fn = std::uintptr_t( __fastcall* )( std::uintptr_t, std::uintptr_t );

	struct functions
	{
		get_base_fn get_base{};
		get_cmd_fn get_cmd{};
		get_angles_fn get_angles{};
		set_angles_fn set_angles{};
		string_copy_fn string_copy{};
		serialize_crc_fn serialize_crc{};
		step_alloc_fn step_alloc{};
		field_push_fn field_push{};
		std::uintptr_t csgo_input{};
		std::uintptr_t client_begin{};
		std::uintptr_t client_end{};
		std::atomic<bool> resolved{};
	};

	functions& fns()
	{
		static functions value{};
		return value;
	}

	struct view_cache
	{
		std::atomic<float> pitch{};
		std::atomic<float> yaw{};
		std::atomic<bool> valid{};
	};

	view_cache& cache()
	{
		static view_cache value{};
		return value;
	}

	[[nodiscard]] bool plausible( std::uintptr_t address )
	{
		return address >= 0x10000 && address < 0x00007FFFFFFFFFFFull;
	}

	// Everything here runs on the game thread inside create_move, on the command the
	// game is building right now. The process API is ReadProcessMemory/WriteProcessMemory:
	// a kernel round trip per field, hundreds per rewritten command, which is what took
	// the frame rate down to single digits with anti-aim on. Plain memory access instead.
	template <typename value_t>
	[[nodiscard]] value_t read( std::uintptr_t address )
	{
		value_t value{};
		if ( plausible( address ) )
			std::memcpy( &value, reinterpret_cast<const void*>( address ), sizeof( value_t ) );
		return value;
	}

	template <typename value_t>
	void write( std::uintptr_t address, value_t value )
	{
		if ( plausible( address ) )
			std::memcpy( reinterpret_cast<void*>( address ), &value, sizeof( value_t ) );
	}

	void write_bytes( std::uintptr_t address, const void* source, std::size_t size )
	{
		if ( plausible( address ) )
			std::memcpy( reinterpret_cast<void*>( address ), source, size );
	}

	void set_bits( std::uintptr_t body, std::uint32_t mask )
	{
		write<std::uint32_t>( body, read<std::uint32_t>( body ) | mask );
	}

	[[nodiscard]] std::uintptr_t scan( std::string_view signature )
	{
		return app::context().process.scan_signature( app::context().modules.client, signature );
	}

	[[nodiscard]] std::uintptr_t follow_call( std::uintptr_t site )
	{
		return site ? app::context().process.decode_call( site ) : 0;
	}

	void write_angle( std::uintptr_t raw, float pitch, float yaw )
	{
		if ( !plausible( raw ) )
			return;
		const auto body = raw + k_impl;
		set_bits( body, 0x3u );
		write<float>( body + k_angle_x, pitch );
		write<float>( body + k_angle_y, yaw );
	}

	void rotate_pair( std::uintptr_t forward_at, std::uintptr_t left_at, float cosine, float sine )
	{
		const auto forward = read<float>( forward_at );
		const auto left = read<float>( left_at );
		if ( forward == 0.0f && left == 0.0f )
			return;
		write<float>( forward_at, forward * cosine + left * sine );
		write<float>( left_at, -forward * sine + left * cosine );
	}

	// Old behaviour, kept for when the movement services cannot be read: rotate
	// the absolute move and every subtick step in place.
	void rotate_movement( std::uintptr_t base, float cosine, float sine )
	{
		rotate_pair( base + k_base_forward, base + k_base_left, cosine, sine );

		const auto field = base + k_base_subticks;
		const auto count = read<int>( field + 0x08 );
		const auto rep = read<std::uintptr_t>( field + 0x10 );
		if ( count <= 0 || count > 64 || !plausible( rep ) )
			return;
		for ( int i = 0; i < count; ++i )
		{
			const auto step = read<std::uintptr_t>( rep + 0x08 + static_cast<std::uintptr_t>( i ) * 8 );
			if ( !plausible( step ) )
				continue;
			const auto body = step + k_impl;
			rotate_pair( body + k_step_forward, body + k_step_left, cosine, sine );
		}
	}

	// CInButtonState that the client mirrors into the protobuf. Only trusted
	// while its vtable points into client.dll, so a layout change cannot make us
	// press random buttons.
	[[nodiscard]] bool cmd_buttons_valid( std::uintptr_t cmd )
	{
		const auto& f = fns();
		const auto vtable = read<std::uintptr_t>( cmd + k_cmd_buttons );
		return f.client_begin && vtable >= f.client_begin && vtable < f.client_end;
	}

	[[nodiscard]] std::uintptr_t button_pb( std::uintptr_t base )
	{
		const auto raw = read<std::uintptr_t>( base + k_base_buttons );
		return plausible( raw ) ? raw + k_impl : 0;
	}

	struct button_set
	{
		std::uint64_t held{};
		std::uint64_t changed{};
		std::uint64_t scroll{};
	};

	[[nodiscard]] bool read_buttons( std::uintptr_t cmd, std::uintptr_t base, button_set& out )
	{
		if ( cmd_buttons_valid( cmd ) )
		{
			out.held = read<std::uint64_t>( cmd + k_cmd_buttons + 0x08 );
			out.changed = read<std::uint64_t>( cmd + k_cmd_buttons + 0x10 );
			out.scroll = read<std::uint64_t>( cmd + k_cmd_buttons + 0x18 );
			return true;
		}
		const auto pb = base ? button_pb( base ) : 0;
		if ( !pb )
			return false;
		out.held = read<std::uint64_t>( pb + k_buttons_state );
		out.changed = read<std::uint64_t>( pb + k_buttons_state + 0x08 );
		out.scroll = read<std::uint64_t>( pb + k_buttons_state + 0x10 );
		return true;
	}

	// Writes the set into CUserCmd::buttons and the protobuf copy that is sent
	// and hashed, so both agree with the move CRC.
	void store_buttons( std::uintptr_t cmd, std::uintptr_t base, const button_set& value )
	{
		if ( cmd_buttons_valid( cmd ) )
		{
			write<std::uint64_t>( cmd + k_cmd_buttons + 0x08, value.held );
			write<std::uint64_t>( cmd + k_cmd_buttons + 0x10, value.changed );
			write<std::uint64_t>( cmd + k_cmd_buttons + 0x18, value.scroll );
		}
		if ( const auto pb = button_pb( base ) )
		{
			set_bits( pb, 0x7u );
			write<std::uint64_t>( pb + k_buttons_state, value.held );
			write<std::uint64_t>( pb + k_buttons_state + 0x08, value.changed );
			write<std::uint64_t>( pb + k_buttons_state + 0x10, value.scroll );
			set_bits( base, 0x2u );
		}
	}

	// The server replays button presses from the subtick steps as well as from the
	// button state, so a press taken out of the state has to leave the steps too.
	// The step stays (it may carry analog movement); only its button goes.
	void drop_button_steps( std::uintptr_t base, std::uint64_t mask )
	{
		const auto field = base + k_base_subticks;
		const auto count = read<int>( field + 0x08 );
		const auto rep = read<std::uintptr_t>( field + 0x10 );
		if ( count <= 0 || count > 64 || !plausible( rep ) )
			return;
		for ( int i = 0; i < count; ++i )
		{
			const auto step = read<std::uintptr_t>( rep + 0x08 + static_cast<std::uintptr_t>( i ) * 8 );
			if ( !plausible( step ) )
				continue;
			const auto body = step + k_impl;
			if ( ( read<std::uint64_t>( body + k_step_button ) & mask ) == 0u )
				continue;
			write<std::uint64_t>( body + k_step_button, 0u );
			write<bool>( body + k_step_pressed, false );
		}
	}

	// Empties the subtick move list and hands back one cleared step for the
	// rebuilt analog move. Cleared elements stay allocated in the repeated
	// field, so the first one is reused before anything is allocated.
	[[nodiscard]] std::uintptr_t single_subtick_step( std::uintptr_t base )
	{
		const auto field = base + k_base_subticks;
		write<int>( field + 0x08, 0 );

		std::uintptr_t raw{};
		const auto rep = read<std::uintptr_t>( field + 0x10 );
		if ( plausible( rep ) && read<int>( rep ) > 0 )
		{
			raw = read<std::uintptr_t>( rep + 0x08 );
			if ( plausible( raw ) )
				write<int>( field + 0x08, 1 );
			else
				raw = 0;
		}
		if ( !raw )
		{
			const auto& f = fns();
			if ( !f.step_alloc || !f.field_push )
				return 0;
			const auto arena = read<std::uintptr_t>( field );
			raw = reinterpret_cast<std::uintptr_t>( f.step_alloc( arena ) );
			if ( !plausible( raw ) )
				return 0;
			f.field_push( field, raw );
		}

		const auto body = raw + k_impl;
		std::uint8_t cleared[ 0x28 ]{};
		write_bytes( body, cleared, sizeof( cleared ) );
		return body;
	}

	// The server builds the move from the subtick steps: key presses at their
	// sub-tick time plus analog deltas against the previous command. Rotating the
	// deltas in place does not rotate the move, so with a changed yaw the player
	// walked towards the fake angle and lost speed in the air. The steps are
	// replaced by one analog step carrying the rotated move instead.
	// Sets the move of this command to (moved_forward, moved_left) as one analog
	// subtick step against the move the previous command left in the movement
	// services. False when that previous move or a step could not be had.
	[[nodiscard]] bool write_move( std::uintptr_t cmd, std::uintptr_t base,
		float moved_forward, float moved_left, bool grounded )
	{
		const auto pawn = game::local_player().pawn();
		const auto services = pawn ? read<std::uintptr_t>( pawn
			+ SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_id ) ) : 0;
		const auto previous_forward_at = SCHEMA( "CPlayer_MovementServices", "m_flCmdForwardMove"_id );
		const auto previous_left_at = SCHEMA( "CPlayer_MovementServices", "m_flCmdLeftMove"_id );
		if ( !plausible( services ) || previous_forward_at <= 0 || previous_left_at <= 0 )
			return false;

		const auto previous_forward = read<float>( services + previous_forward_at );
		const auto previous_left = read<float>( services + previous_left_at );
		if ( !std::isfinite( previous_forward ) || !std::isfinite( previous_left ) )
			return false;

		const auto step = single_subtick_step( base );
		if ( !step )
			return false;

		moved_forward = std::clamp( moved_forward, -1.0f, 1.0f );
		moved_left = std::clamp( moved_left, -1.0f, 1.0f );
		set_bits( base, 0x40u | 0x80u );
		write<float>( base + k_base_forward, moved_forward );
		write<float>( base + k_base_left, moved_left );

		// button, pressed, when, analog forward, analog left.
		write<std::uint32_t>( step, 0x1u | 0x2u | 0x4u | 0x8u | 0x10u );
		write<float>( step + k_step_forward, moved_forward - previous_forward );
		write<float>( step + k_step_left, moved_left - previous_left );

		if ( !grounded )
			return true;
		button_set buttons{};
		if ( !read_buttons( cmd, base, buttons ) )
			return true;
		buttons.held &= ~( in_forward | in_back | in_moveleft | in_moveright );
		if ( moved_forward > 0.0f ) buttons.held |= in_forward;
		else if ( moved_forward < 0.0f ) buttons.held |= in_back;
		if ( moved_left > 0.0f ) buttons.held |= in_moveleft;
		else if ( moved_left < 0.0f ) buttons.held |= in_moveright;
		store_buttons( cmd, base, buttons );
		return true;
	}

	void fix_movement( std::uintptr_t cmd, std::uintptr_t base, float from_yaw, float to_yaw, bool grounded )
	{
		const auto delta = foundation::to_radians( foundation::wrap_yaw( to_yaw - from_yaw ) );
		const auto cosine = std::cos( delta );
		const auto sine = std::sin( delta );

		const auto forward = read<float>( base + k_base_forward );
		const auto left = read<float>( base + k_base_left );
		if ( !write_move( cmd, base, forward * cosine + left * sine, -forward * sine + left * cosine, grounded ) )
			rotate_movement( base, cosine, sine );
	}

	// Mirrors the client's own move CRC: buttons (field 3) and view angles
	// (field 4) of the base command serialised and hashed into move_crc.
	void sign( std::uintptr_t base )
	{
		auto& f = fns();
		if ( !f.serialize_crc )
			return;

		std::uint64_t states[ 3 ]{};
		const auto buttons = read<std::uintptr_t>( base + k_base_buttons );
		if ( plausible( buttons ) )
			for ( int i = 0; i < 3; ++i )
				states[ i ] = read<std::uint64_t>( buttons + k_impl + k_buttons_state + i * 8 );

		float angles[ 3 ]{};
		const auto angle_raw = read<std::uintptr_t>( base + k_base_angles );
		if ( plausible( angle_raw ) )
		{
			angles[ 0 ] = read<float>( angle_raw + k_impl + k_angle_x );
			angles[ 1 ] = read<float>( angle_raw + k_impl + k_angle_y );
			angles[ 2 ] = read<float>( angle_raw + k_impl + k_angle_z );
		}

		std::uint8_t buffer[ 64 ]{};
		auto cursor = buffer;
		std::uint8_t button_size = 0;
		for ( const auto state : states )
			if ( state )
				button_size += 9;
		if ( button_size )
		{
			*cursor++ = 0x1a;
			*cursor++ = button_size;
			const std::uint8_t tags[ 3 ]{ 0x09, 0x11, 0x19 };
			for ( int i = 0; i < 3; ++i )
			{
				if ( !states[ i ] )
					continue;
				*cursor++ = tags[ i ];
				std::memcpy( cursor, &states[ i ], 8 );
				cursor += 8;
			}
		}

		std::uint8_t angle_size = 0;
		for ( const auto angle : angles )
			if ( angle != 0.0f )
				angle_size += 5;
		if ( angle_size )
		{
			*cursor++ = 0x22;
			*cursor++ = angle_size;
			const std::uint8_t tags[ 3 ]{ 0x0d, 0x15, 0x1d };
			for ( int i = 0; i < 3; ++i )
			{
				if ( angles[ i ] == 0.0f )
					continue;
				*cursor++ = tags[ i ];
				std::memcpy( cursor, &angles[ i ], 4 );
				cursor += 4;
			}
		}

		set_bits( base, 0x1u );
		const auto raw = base - k_impl;
		auto arena = read<std::uintptr_t>( raw + 0x08 );
		const auto indirect = ( arena & 1 ) != 0;
		arena &= ~0x3ull;
		if ( indirect )
			arena = read<std::uintptr_t>( arena );

		// The setter takes an MSVC std::string (0x20 bytes) and copies it into the arena.
		// Laid out in long mode over our own buffer: nothing is allocated, so nothing is
		// leaked per command, and nothing is written past a too-small temporary.
		struct msvc_string
		{
			const std::uint8_t* data;
			std::uint64_t unused;
			std::uint64_t size;
			std::uint64_t capacity;
		};
		static_assert( sizeof( msvc_string ) == 0x20 );
		msvc_string message{ buffer, 0, static_cast<std::uint64_t>( cursor - buffer ), sizeof( buffer ) - 1 };
		f.serialize_crc( reinterpret_cast<void*>( base + k_base_crc ), &message, arena );
	}

} // namespace

void resolve_once();

bool resolve()
{
	static std::once_flag once;
	std::call_once( once, resolve_once );
	return fns().resolved;
}

void resolve_once()
{
	auto& f = fns();

	f.get_base = reinterpret_cast<get_base_fn>( follow_call(
		[] { const auto s = scan( "48 83 EC 28 E8 ? ? ? ? 8B 80 10 59 00 00" ); return s ? s + 4 : 0; }() ) );
	f.get_cmd = reinterpret_cast<get_cmd_fn>( scan( "40 53 48 83 EC 20 8B DA E8 ? ? ? ? 4C 8B C0" ) );
	f.get_angles = reinterpret_cast<get_angles_fn>( follow_call(
		[] { const auto s = scan( "8B 0D ? ? ? ? 8B D3 E8 ? ? ? ? F2 0F 10 00" ); return s ? s + 8 : 0; }() ) );
	f.set_angles = reinterpret_cast<set_angles_fn>( scan( "85 D2 75 ? 48 63 81" ) );
	f.string_copy = reinterpret_cast<string_copy_fn>( follow_call( scan( "E8 ? ? ? ? 0F 10 45 88" ) ) );
	f.serialize_crc = reinterpret_cast<serialize_crc_fn>( scan(
		"48 89 5C 24 ? 55 56 57 48 83 EC 30 49 8B C0 48 8B FA 48 8B F1 48 8B 09 F6 C1 03" ) );
	if ( const auto site = scan( "84 C0 74 0C 48 8D 0D ? ? ? ? E8 ? ? ? ?" ) )
		f.csgo_input = app::context().process.decode_rip( site + 4, 3, 7 );
	f.step_alloc = reinterpret_cast<step_alloc_fn>( follow_call( [] {
		const auto s = scan( "48 8B 54 CA 08 8D 41 01 89 47 08 EB 16 48 8B 0F E8 ? ? ? ? 48 8B D0 48 8B CF" );
		return s ? s + 16 : 0;
	}() ) );
	f.field_push = reinterpret_cast<field_push_fn>( follow_call( scan( "E8 ? ? ? ? 4C 8B D0 45 8B 4A 10" ) ) );

	const auto client = app::context().modules.client;
	const auto client_size = client ? app::context().process.module_image_size( client ) : 0;
	f.client_begin = client_size ? client : 0;
	f.client_end = client_size ? client + client_size : 0;

	f.resolved = f.get_base && f.get_cmd;
	auto& log = app::context().diagnostics;
	log.info( "[usercmd] base={} cmd={} get_va={} set_va={} crc={}/{} step={}/{} input={:#x}",
		f.get_base != nullptr, f.get_cmd != nullptr, f.get_angles != nullptr, f.set_angles != nullptr,
		f.string_copy != nullptr, f.serialize_crc != nullptr, f.step_alloc != nullptr,
		f.field_push != nullptr, f.csgo_input );
}

bool ready()
{
	return fns().resolved;
}

std::uintptr_t current( std::uintptr_t controller )
{
	auto& f = fns();
	if ( !f.resolved || !plausible( controller ) )
		return 0;
	const auto base = f.get_base( controller );
	if ( !plausible( base ) )
		return 0;
	const auto sequence = read<int>( base + 0x5910 );
	if ( sequence <= 0 )
		return 0;
	const auto cmd = f.get_cmd( controller, sequence );
	return plausible( cmd ) ? cmd : 0;
}

bool view_angles( foundation::vec3& out )
{
	auto& f = fns();
	if ( !f.get_angles || !plausible( f.csgo_input ) )
		return false;
	const auto angles = f.get_angles( f.csgo_input, 0 );
	if ( !angles || !std::isfinite( angles[ 0 ] ) || !std::isfinite( angles[ 1 ] ) )
		return false;
	out = { angles[ 0 ], angles[ 1 ], angles[ 2 ] };
	return true;
}

bool set_view_angles( const foundation::vec3& angles )
{
	auto& f = fns();
	if ( !f.set_angles || !plausible( f.csgo_input ) )
		return false;
	const float value[ 3 ]{ std::clamp( angles.x, -89.0f, 89.0f ), foundation::wrap_yaw( angles.y ), 0.0f };
	f.set_angles( f.csgo_input, 0, value );
	return true;
}

bool write_angles( std::uintptr_t cmd, float pitch, float yaw, const write_options& options )
{
	if ( !plausible( cmd ) || !std::isfinite( pitch ) || !std::isfinite( yaw ) )
		return false;
	const auto raw_base = read<std::uintptr_t>( cmd + k_cmd_base );
	if ( !plausible( raw_base ) )
		return false;
	const auto base = raw_base + k_impl;
	const auto angle_raw = read<std::uintptr_t>( base + k_base_angles );
	if ( !plausible( angle_raw ) )
		return false;

	pitch = std::clamp( pitch, -89.0f, 89.0f );
	yaw = foundation::wrap_yaw( yaw );
	const auto old_yaw = read<float>( angle_raw + k_impl + k_angle_y );

	set_bits( base, 0x4u );
	write_angle( angle_raw, pitch, yaw );

	const auto history = cmd + k_cmd_history;
	const auto count = read<int>( history + 0x08 );
	const auto rep = read<std::uintptr_t>( history + 0x10 );
	if ( count > 0 && count <= 64 && plausible( rep ) )
	{
		for ( int i = 0; i < count; ++i )
		{
			const auto entry = read<std::uintptr_t>( rep + 0x08 + static_cast<std::uintptr_t>( i ) * 8 );
			if ( !plausible( entry ) )
				continue;
			const auto body = entry + k_impl;
			set_bits( body, 0x1u );
			write_angle( read<std::uintptr_t>( body + k_history_angles ), pitch, yaw );
		}
	}

	if ( options.fix_movement && std::isfinite( old_yaw ) )
		fix_movement( cmd, base, old_yaw, yaw, options.grounded );

	if ( options.attack )
	{
		// The shot is pressed inside this very command, so it leaves with the
		// angles written above instead of whichever command a real click hits.
		button_set buttons{};
		if ( read_buttons( cmd, base, buttons ) )
		{
			buttons.held |= in_attack;
			buttons.changed |= in_attack;
			buttons.scroll |= in_attack;
			store_buttons( cmd, base, buttons );
		}
		if ( count > 0 && count <= 64 )
		{
			set_bits( cmd + 0x20, 0x20u );
			write<int>( cmd + k_cmd_attack1, count - 1 );
		}
	}
	else if ( options.fix_movement )
	{
		// Keeps the protobuf buttons in step with CUserCmd::buttons before the
		// CRC is taken over them.
		button_set buttons{};
		if ( cmd_buttons_valid( cmd ) && read_buttons( cmd, base, buttons ) )
			store_buttons( cmd, base, buttons );
	}
	sign( base );
	return true;
}

bool set_button( std::uintptr_t cmd, std::uint64_t mask, bool down, bool was_down )
{
	if ( !plausible( cmd ) )
		return false;
	const auto raw_base = read<std::uintptr_t>( cmd + k_cmd_base );
	if ( !plausible( raw_base ) )
		return false;
	const auto base = raw_base + k_impl;
	button_set set{};
	if ( !read_buttons( cmd, base, set ) )
		return false;
	const auto held = down ? ( set.held | mask ) : ( set.held & ~mask );
	const auto changed = down != was_down ? ( set.changed | mask ) : ( set.changed & ~mask );
	if ( held == set.held && changed == set.changed )
		return true;
	set.held = held;
	set.changed = changed;
	store_buttons( cmd, base, set );
	if ( !down )
		drop_button_steps( base, mask );
	sign( base );
	return true;
}

bool set_move( std::uintptr_t cmd, float forward, float left )
{
	if ( !plausible( cmd ) || !std::isfinite( forward ) || !std::isfinite( left ) )
		return false;
	const auto raw_base = read<std::uintptr_t>( cmd + k_cmd_base );
	if ( !plausible( raw_base ) )
		return false;
	const auto base = raw_base + k_impl;
	if ( !write_move( cmd, base, forward, left, true ) )
		return false;
	sign( base );
	return true;
}

bool buttons( std::uintptr_t cmd, std::uint64_t& out )
{
	if ( !plausible( cmd ) )
		return false;
	const auto raw_base = read<std::uintptr_t>( cmd + k_cmd_base );
	const auto base = plausible( raw_base ) ? raw_base + k_impl : 0;
	button_set set{};
	if ( !read_buttons( cmd, base, set ) )
		return false;
	out = set.held;
	return true;
}

bool cached_view_angles( foundation::vec3& out )
{
	auto& c = cache();
	if ( !c.valid.load( std::memory_order_acquire ) )
		return false;
	out = { c.pitch.load( std::memory_order_relaxed ), c.yaw.load( std::memory_order_relaxed ), 0.0f };
	return true;
}

void publish_view_angles()
{
	foundation::vec3 angles{};
	if ( !view_angles( angles ) )
		return;
	auto& c = cache();
	c.pitch.store( angles.x, std::memory_order_relaxed );
	c.yaw.store( angles.y, std::memory_order_relaxed );
	c.valid.store( true, std::memory_order_release );
}

} // namespace features::combat::usercmd
