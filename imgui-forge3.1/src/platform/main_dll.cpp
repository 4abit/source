// Injected build: the cheat lives inside cs2.exe. No window, no external
// overlay — a swap-chain Present hook draws the Neverlose menu, the ESP and the
// chams straight into the game frame.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <thread>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "menu/blur.h"
#include "menu/boot.h"
#include "menu/esp_model.h"
#include "menu/fonts.h"
#include "menu/menu.h"
#include "menu/model_preview.h"
#include "menu/sky_list.h"
#include "menu/style.h"
#include "platform/blur_dx11.h"

// Must precede the state headers: libc++ has no std::atomic<std::shared_ptr<T>>.
#include <port/atomic_shared.hpp>
#include <app/context.hpp>
#include <core/input/hotkeys.hpp>
#include <core/state/services.hpp>
#include <core/state/pose.hpp>
#include <port/cheat_bridge.hpp>
#include <port/config_bridge.hpp>
#include <port/hook_hud.hpp>
#include <port/preload.hpp>
#include <render/chams/renderer.hpp>
#include <render/menu/menu.hpp>
#include <render/overlay/input.hpp>
#include <render/overlay/overlay.hpp>

namespace {

	using present_fn = HRESULT( STDMETHODCALLTYPE* )( IDXGISwapChain*, UINT, UINT );

	constexpr std::uint32_t k_vtable_present_index = 8;

	// `jmp qword ptr [rip+0]` followed by the absolute destination: 14 bytes,
	// which always fit at a function entry on x64.
	constexpr std::size_t k_patch_size = 14;

	// Lifecycle diagnostics land next to the vesta runtime area so an in-game
	// failure is visible without a debugger.
	void log_event( const char* text )
	{
		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return;
		const auto directory = std::filesystem::path( temporary ) / L"vesta";
		std::error_code error{};
		std::filesystem::create_directories( directory, error );
		std::ofstream stream( directory / L"internal.log", std::ios::app );
		if ( !stream )
			return;
		stream << "tick=" << ::GetTickCount64() << " pid=" << ::GetCurrentProcessId()
			<< " " << text << '\n';
	}

	HMODULE g_module = nullptr;
	// The genuine dxgi Present. Everything routes through its code, never
	// through a vtable entry (see the detour notes below).
	present_fn g_real_present = nullptr;
	std::atomic<bool> g_installed{ false };
	std::atomic<bool> g_ready{ false };
	std::atomic<bool> g_suppressed{ false };
	std::atomic<bool> g_insert_down{ false };
	std::atomic<bool> g_exit_down{ false };
	std::atomic<std::uint64_t> g_frame_index{ 0 };

	// Pinpoints the stage that kills the game: dense for the first frames, then
	// sparse so the log stays readable.
	std::atomic<const char*> g_last_stage{ "startup" };

	void log_frame_stage( const std::uint64_t frame, const char* stage )
	{
		g_last_stage.store( stage, std::memory_order_relaxed );
		if ( frame > 40 && ( frame % 500 ) != 0 )
			return;
		char text[ 128 ]{};
		std::snprintf( text, sizeof( text ), "frame=%llu %s",
			static_cast<unsigned long long>( frame ), stage );
		log_event( text );
	}

	// A fault inside the injected module used to look like a silent game exit: the
	// process dies and the log simply stops. This handler writes the exception code,
	// the faulting module with its offset and the last frame stage, then lets the
	// normal handling continue. It allocates nothing, so it is safe on a broken stack.
	LONG WINAPI crash_filter( EXCEPTION_POINTERS* info )
	{
		static std::atomic<bool> reported{ false };
		if ( reported.exchange( true, std::memory_order_acq_rel ) )
			return EXCEPTION_CONTINUE_SEARCH;
		if ( !info || !info->ExceptionRecord )
			return EXCEPTION_CONTINUE_SEARCH;

		wchar_t directory[ MAX_PATH ]{};
		if ( !::GetTempPathW( MAX_PATH, directory ) )
			return EXCEPTION_CONTINUE_SEARCH;
		::lstrcatW( directory, L"vesta" );
		::CreateDirectoryW( directory, nullptr );
		::lstrcatW( directory, L"\\crash.log" );

		const HANDLE file = ::CreateFileW( directory, FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr );
		if ( file == INVALID_HANDLE_VALUE )
			return EXCEPTION_CONTINUE_SEARCH;

		const auto* record = info->ExceptionRecord;
		// First-chance exceptions that the runtime or the game handles itself are not
		// crashes: a C++ throw, a breakpoint or a debug print would otherwise fill the
		// file with noise and hide the real fault.
		switch ( record->ExceptionCode )
		{
		case 0xE06D7363: // C++ exception
		case 0x80000003: // breakpoint
		case 0x40010006: // OutputDebugString
		case 0x406D1388: // thread name
			return EXCEPTION_CONTINUE_SEARCH;
		default: break;
		}

		const auto address = reinterpret_cast<std::uintptr_t>( record->ExceptionAddress );
		wchar_t module[ MAX_PATH ]{};
		std::uintptr_t base = 0;
		HMODULE owner = nullptr;
		if ( ::GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
				| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>( address ), &owner ) && owner )
		{
			base = reinterpret_cast<std::uintptr_t>( owner );
			::GetModuleFileNameW( owner, module, MAX_PATH );
		}

		// snprintf instead of wsprintf: the latter has no %llu / %p, which desynchronised
		// every field of the line. Non-ASCII module characters become '?'.
		char narrow[ MAX_PATH ]{};
		for ( int index = 0; index < MAX_PATH - 1 && module[ index ]; ++index )
			narrow[ index ] = module[ index ] < 128 ? static_cast<char>( module[ index ] ) : '?';

		char line[ 1024 ]{};
		const auto count = std::snprintf( line, sizeof( line ),
			"tick=%llu pid=%lu code=0x%08lX flags=0x%08lX address=0x%llX %s+0x%llX stage=%s\r\n",
			static_cast<unsigned long long>( ::GetTickCount64() ),
			static_cast<unsigned long>( ::GetCurrentProcessId() ),
			static_cast<unsigned long>( record->ExceptionCode ),
			static_cast<unsigned long>( record->ExceptionFlags ),
			static_cast<unsigned long long>( address ), narrow,
			static_cast<unsigned long long>( address - base ),
			g_last_stage.load( std::memory_order_relaxed ) );
		if ( count > 0 )
		{
			DWORD written = 0;
			::WriteFile( file, line,
				static_cast<DWORD>( count < static_cast<int>( sizeof( line ) ) ? count : sizeof( line ) - 1 ),
				&written, nullptr );
		}
		::CloseHandle( file );
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Address-inside-module test; used to tell a real dxgi entry from a foreign
	// overlay hook sitting in the same vtable slot.
	bool inside_module( const std::uintptr_t address, const wchar_t* name )
	{
		const auto base = reinterpret_cast<std::uintptr_t>(
			::GetModuleHandleW( name ) );
		if ( address == 0 || base == 0 )
			return false;

		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( base );
		if ( dos->e_magic != IMAGE_DOS_SIGNATURE )
			return false;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			base + dos->e_lfanew );
		if ( nt->Signature != IMAGE_NT_SIGNATURE )
			return false;
		return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
	}

	// The loader probes the genuine Present in its own clean process and leaves
	// the RVA here; dxgi.dll sits at the same base in every process.
	std::uintptr_t read_present_rva()
	{
		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return 0;
		const auto path = std::filesystem::path( temporary ) / L"vesta"
			/ L"present_rva.txt";
		std::ifstream stream( path );
		if ( !stream )
			return 0;
		std::uintptr_t offset = 0;
		stream >> std::hex >> offset;
		return offset;
	}

	// Returns the true Present, or nullptr when only a foreign hook is available
	// (which must never be called, see hooked_present).
	present_fn resolve_real_present( const std::uintptr_t slot_entry )
	{
		const auto dxgi = reinterpret_cast<std::uintptr_t>(
			::GetModuleHandleW( L"dxgi.dll" ) );
		const auto offset = read_present_rva();
		if ( dxgi != 0 && offset != 0 )
		{
			const auto candidate = dxgi + offset;
			if ( inside_module( candidate, L"dxgi.dll" ) )
				return reinterpret_cast<present_fn>( candidate );
		}
		if ( inside_module( slot_entry, L"dxgi.dll" ) )
			return reinterpret_cast<present_fn>( slot_entry );
		return nullptr;
	}

	HRESULT STDMETHODCALLTYPE hooked_present( IDXGISwapChain* swap_chain,
		UINT sync_interval, UINT flags );

	// ---------------------------------------------------------------- detour
	// Present is hooked by patching dxgi's own code, NOT the IDXGISwapChain
	// vtable. The shared table is contested ground: Steam's overlay installs its
	// hook there and rebuilds its chain from whatever it finds, so an internal
	// hook sitting in that slot — or a private copy of it — makes the overlay
	// call itself until the stack overflows (c00000fd, proven from crash dumps
	// both ways). Code-level patching leaves every table pristine and the
	// overlay keeps working.
	void write_code( void* where, const std::uint8_t* bytes, const std::size_t count )
	{
		DWORD previous = 0;
		if ( !::VirtualProtect( where, count, PAGE_EXECUTE_READWRITE, &previous ) )
			return;
		std::memcpy( where, bytes, count );
		::FlushInstructionCache( ::GetCurrentProcess(), where, count );
		DWORD restored = 0;
		::VirtualProtect( where, count, previous, &restored );
	}

	// 14-byte absolute jump at a function entry. Calling the original means
	// lifting the patch, calling, and putting it back, so the original runs from
	// its own first byte and no trampoline (nor an instruction-length decoder)
	// is required.
	struct detour
	{
		std::uint8_t* target{};
		std::uint8_t original[ k_patch_size ]{};
		std::uint8_t patch[ k_patch_size ]{};
		bool armed{};

		bool install( void* destination, void* hook )
		{
			if ( !destination || !hook )
				return false;
			target = reinterpret_cast<std::uint8_t*>( destination );
			std::memcpy( original, target, k_patch_size );
			patch[ 0 ] = 0xFF; // jmp qword ptr [rip+0]
			patch[ 1 ] = 0x25;
			std::memset( patch + 2, 0, 4 );
			const auto address = reinterpret_cast<std::uintptr_t>( hook );
			std::memcpy( patch + 6, &address, sizeof( address ) );
			this->rearm( );
			return true;
		}

		void lift( )
		{
			if ( target )
				write_code( target, original, k_patch_size );
		}

		void rearm( )
		{
			if ( target )
				write_code( target, patch, k_patch_size );
			armed = target != nullptr;
		}

		void remove( )
		{
			if ( armed )
				this->lift( );
			armed = false;
		}
	};

	detour g_present_detour{};
	detour g_raw_input_detour{};
	detour g_raw_input_buffer_detour{};
	detour g_set_cursor_pos_detour{};
	detour g_clip_cursor_detour{};
	detour g_show_cursor_detour{};
	detour g_set_cursor_detour{};
	std::atomic<bool> g_capture_input{ false };
	// Side-button macros arrive as real keyboard packets. CS2 reads them through raw
	// input (RIDEV_NOLEGACY), so swallowing WM_KEY* does not stop the characters.
	std::atomic<bool> g_raw_x1{ false };
	std::atomic<bool> g_raw_x2{ false };
	std::atomic<bool> g_async_side_seen{ false };
	HWND g_macro_hwnd = nullptr;
	WNDPROC g_macro_prev = nullptr;

	// Forward declaration for cursor functions that need g_window before it is defined
	extern HWND g_window;

	// In a live match CS2 takes the mouse with raw input and RIDEV_NOLEGACY: Windows
	// then stops moving the cursor at all, so GetCursorPos freezes and a menu cursor
	// driven by it can never move. While the menu owns the mouse we therefore track
	// the raw deltas ourselves, drive the cursor from them, and swallow the game's
	// own centering and clipping. Also force the cursor visible and stop the game
	// from hiding it or setting a null cursor.
	using set_cursor_pos_fn = BOOL( WINAPI* )( int, int );
	using clip_cursor_fn = BOOL( WINAPI* )( const RECT* );
	using show_cursor_fn = int( WINAPI* )( BOOL );
	using set_cursor_fn = HCURSOR( WINAPI* )( HCURSOR );
	set_cursor_pos_fn g_real_set_cursor_pos = nullptr;
	clip_cursor_fn g_real_clip_cursor = nullptr;
	show_cursor_fn g_real_show_cursor = nullptr;
	set_cursor_fn g_real_set_cursor = nullptr;
	std::atomic<int> g_cursor_x{ 0 };
	std::atomic<int> g_cursor_y{ 0 };
	std::atomic<bool> g_cursor_tracked{ false };
	std::atomic<int> g_show_cursor_calls{ 0 };
	RECT g_game_clip_rect{};
	bool g_game_clip_valid{ false };

	void place_cursor( const int x, const int y )
	{
		g_cursor_x.store( x, std::memory_order_relaxed );
		g_cursor_y.store( y, std::memory_order_relaxed );
		if ( !g_real_set_cursor_pos )
			return;
		g_set_cursor_pos_detour.lift( );
		g_real_set_cursor_pos( x, y );
		g_set_cursor_pos_detour.rearm( );
	}

	void clamp_cursor_to_window( HWND window, int& x, int& y )
	{
		if ( !window )
			return;
		RECT client{};
		if ( !::GetClientRect( window, &client ) )
			return;
		POINT top_left{ client.left, client.top };
		if ( !::ClientToScreen( window, &top_left ) )
			return;
		const int width = client.right - client.left;
		const int height = client.bottom - client.top;
		if ( width <= 0 || height <= 0 )
			return;
		x = std::clamp( x, static_cast<int>( top_left.x ), static_cast<int>( top_left.x ) + width - 1 );
		y = std::clamp( y, static_cast<int>( top_left.y ), static_cast<int>( top_left.y ) + height - 1 );
	}

	void begin_cursor_capture( )
	{
		// SDL3 clips the cursor to the client rect in relative mode and only releases
		// it through ClipCursor(NULL). The game rarely repeats that call, so an
		// inherited clip rectangle used to survive the whole menu session: Windows
		// clamped every position the menu cursor asked for and the pointer looked
		// frozen even though the deltas were being tracked.
		if ( g_real_clip_cursor )
		{
			g_clip_cursor_detour.lift( );
			g_real_clip_cursor( nullptr );
			g_clip_cursor_detour.rearm( );
		}
		// CS2 hides the cursor by driving the display counter negative, usually once,
		// long before the menu opens. Nothing would bring it back on its own, so the
		// counter is pushed back to >= 0 here and every forced increment is recorded
		// for end_cursor_capture() to undo.
		if ( g_real_show_cursor )
		{
			g_show_cursor_detour.lift( );
			int displayed = g_real_show_cursor( TRUE );
			int forced = 1;
			while ( displayed < 0 && forced < 64 )
			{
				displayed = g_real_show_cursor( TRUE );
				++forced;
			}
			g_show_cursor_detour.rearm( );
			g_show_cursor_calls.fetch_add( forced, std::memory_order_relaxed );
		}
		POINT cursor{};
		if ( ::GetCursorPos( &cursor ) )
		{
			g_cursor_x.store( cursor.x, std::memory_order_relaxed );
			g_cursor_y.store( cursor.y, std::memory_order_relaxed );
		}
		g_cursor_tracked.store( true, std::memory_order_relaxed );
	}

	void end_cursor_capture( )
	{
		g_cursor_tracked.store( false, std::memory_order_relaxed );
		// Give the game its clipping back.
		if ( g_real_clip_cursor )
		{
			g_clip_cursor_detour.lift( );
			g_real_clip_cursor( g_game_clip_valid ? &g_game_clip_rect : nullptr );
			g_clip_cursor_detour.rearm( );
		}
		// Restore the cursor visibility: call ShowCursor(FALSE) as many times as
		// we called ShowCursor(TRUE) to balance the display counter.
		const int calls = g_show_cursor_calls.exchange( 0, std::memory_order_relaxed );
		if ( g_real_show_cursor )
		{
			g_show_cursor_detour.lift( );
			for ( int i = 0; i < calls; ++i )
				g_real_show_cursor( FALSE );
			g_show_cursor_detour.rearm( );
		}
	}

	void apply_mouse_delta( const LONG dx, const LONG dy )
	{
		if ( !g_cursor_tracked.load( std::memory_order_relaxed ) )
			begin_cursor_capture( );

		int x = g_cursor_x.load( std::memory_order_relaxed ) + static_cast<int>( dx );
		int y = g_cursor_y.load( std::memory_order_relaxed ) + static_cast<int>( dy );
		clamp_cursor_to_window( g_window, x, y );
		place_cursor( x, y );
	}

	bool install_present_detour( present_fn target )
	{
		return g_present_detour.install( reinterpret_cast<void*>( target ),
			reinterpret_cast<void*>( &hooked_present ) );
	}

	void remove_present_detour( )
	{
		g_present_detour.remove( );
	}

	// Runs the genuine Present: the patch is lifted for the duration of the
	// call, so the original implementation starts from its own first byte.
	HRESULT call_original_present( IDXGISwapChain* swap_chain,
		const UINT sync_interval, const UINT flags )
	{
		if ( !g_real_present || !g_present_detour.target )
			return S_OK;
		g_present_detour.lift( );
		const HRESULT result = g_real_present( swap_chain, sync_interval, flags );
		g_present_detour.rearm( );
		return result;
	}

	// ------------------------------------------------------------- raw input
	// Swallowing the mouse through a WH_MOUSE_LL hook stops clicks but NOT the
	// camera: CS2 reads mouse look from raw input (WM_INPUT), which never passes
	// through a low-level hook. The queued raw input is drained so it cannot pile
	// up and jump the view the moment the menu closes, and the game is told there
	// was nothing.
	using get_raw_input_data_fn = UINT( WINAPI* )( HRAWINPUT, UINT, LPVOID, PUINT, UINT );
	using get_raw_input_buffer_fn = UINT( WINAPI* )( PRAWINPUT, PUINT, UINT );

	get_raw_input_data_fn g_real_get_raw_input_data = nullptr;
	get_raw_input_buffer_fn g_real_get_raw_input_buffer = nullptr;

	void note_raw_mouse( const RAWMOUSE& mouse )
	{
		const auto flags = mouse.usButtonFlags;
		if ( ( flags & RI_MOUSE_BUTTON_4_DOWN ) != 0 )
			g_raw_x1.store( true, std::memory_order_relaxed );
		if ( ( flags & RI_MOUSE_BUTTON_4_UP ) != 0 )
			g_raw_x1.store( false, std::memory_order_relaxed );
		if ( ( flags & RI_MOUSE_BUTTON_5_DOWN ) != 0 )
			g_raw_x2.store( true, std::memory_order_relaxed );
		if ( ( flags & RI_MOUSE_BUTTON_5_UP ) != 0 )
			g_raw_x2.store( false, std::memory_order_relaxed );
	}

	[[nodiscard]] bool side_button_down( )
	{
		const bool x1 = ( ::GetAsyncKeyState( VK_XBUTTON1 ) & 0x8000 ) != 0;
		const bool x2 = ( ::GetAsyncKeyState( VK_XBUTTON2 ) & 0x8000 ) != 0;
		if ( x1 || x2 )
			g_async_side_seen.store( true, std::memory_order_relaxed );
		// Once the OS has reported the button, trust it on release so a missed
		// raw button-up cannot keep eating digits. If async never sees the
		// button, the raw latch stays until the matching up packet.
		if ( g_async_side_seen.load( std::memory_order_relaxed ) && !x1 && !x2 )
		{
			g_raw_x1.store( false, std::memory_order_relaxed );
			g_raw_x2.store( false, std::memory_order_relaxed );
			return false;
		}
		return x1 || x2
			|| g_raw_x1.load( std::memory_order_relaxed )
			|| g_raw_x2.load( std::memory_order_relaxed );
	}

	[[nodiscard]] bool macro_digit( USHORT vk, USHORT make )
	{
		if ( vk == VK_PACKET )
			return true;
		if ( vk == 0 && make != 0 )
			vk = static_cast<USHORT>( ::MapVirtualKeyW( make, MAPVK_VSC_TO_VK_EX ) );
		return ( vk >= '0' && vk <= '9' )
			|| ( vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 );
	}

	void silence_keyboard( RAWKEYBOARD& keyboard )
	{
		keyboard.MakeCode = 0;
		keyboard.VKey = 0;
		keyboard.Flags = static_cast<USHORT>( keyboard.Flags | RI_KEY_BREAK );
		keyboard.Message = WM_KEYUP;
	}

	void walk_raw_blocks( PRAWINPUT data, UINT blocks, bool silence_keys )
	{
		if ( !data || blocks == 0 || blocks == static_cast<UINT>( -1 ) )
			return;
		constexpr std::uintptr_t k_raw_input_alignment = 8;
		auto* block = data;
		for ( UINT index = 0; block && index < blocks; ++index )
		{
			if ( block->header.dwSize < sizeof( RAWINPUTHEADER ) )
				break;
			if ( block->header.dwType == RIM_TYPEMOUSE )
				note_raw_mouse( block->data.mouse );
			const auto next = reinterpret_cast<std::uintptr_t>( block )
				+ block->header.dwSize;
			block = reinterpret_cast<PRAWINPUT>(
				( next + k_raw_input_alignment - 1 )
					& ~( k_raw_input_alignment - 1 ) );
		}
		if ( !silence_keys || !side_button_down( ) )
			return;
		block = data;
		for ( UINT index = 0; block && index < blocks; ++index )
		{
			if ( block->header.dwSize < sizeof( RAWINPUTHEADER ) )
				break;
			if ( block->header.dwType == RIM_TYPEKEYBOARD
				&& macro_digit( block->data.keyboard.VKey,
					block->data.keyboard.MakeCode ) )
			{
				silence_keyboard( block->data.keyboard );
			}
			const auto next = reinterpret_cast<std::uintptr_t>( block )
				+ block->header.dwSize;
			block = reinterpret_cast<PRAWINPUT>(
				( next + k_raw_input_alignment - 1 )
					& ~( k_raw_input_alignment - 1 ) );
		}
	}

	void silence_raw_packet( RAWINPUT* raw )
	{
		if ( !raw )
			return;
		if ( raw->header.dwType == RIM_TYPEMOUSE )
			note_raw_mouse( raw->data.mouse );
		if ( raw->header.dwType == RIM_TYPEKEYBOARD
			&& side_button_down( )
			&& macro_digit( raw->data.keyboard.VKey, raw->data.keyboard.MakeCode ) )
		{
			silence_keyboard( raw->data.keyboard );
		}
	}

	[[nodiscard]] bool digit_window_message( UINT message, WPARAM param )
	{
		if ( message == WM_CHAR || message == WM_SYSCHAR || message == WM_UNICHAR )
			return param >= '0' && param <= '9';
		return ( param >= '0' && param <= '9' )
			|| ( param >= VK_NUMPAD0 && param <= VK_NUMPAD9 )
			|| param == VK_PACKET;
	}

	LRESULT CALLBACK macro_wndproc( HWND hwnd, UINT message, WPARAM param, LPARAM extra )
	{
		if ( side_button_down( ) )
		{
			switch ( message )
			{
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_CHAR:
			case WM_SYSCHAR:
			case WM_UNICHAR:
				if ( digit_window_message( message, param ) )
					return 0;
				break;
			default:
				break;
			}
		}
		return g_macro_prev
			? ::CallWindowProcW( g_macro_prev, hwnd, message, param, extra )
			: ::DefWindowProcW( hwnd, message, param, extra );
	}

	void install_macro_wndproc( HWND window )
	{
		if ( !window || g_macro_hwnd )
			return;
		HWND root = ::GetAncestor( window, GA_ROOT );
		if ( !root )
			root = window;
		const auto previous = reinterpret_cast<WNDPROC>( ::SetWindowLongPtrW(
			root, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>( &macro_wndproc ) ) );
		if ( !previous )
			return;
		g_macro_prev = previous;
		g_macro_hwnd = root;
	}

	void remove_macro_wndproc( )
	{
		if ( g_macro_hwnd && g_macro_prev )
			::SetWindowLongPtrW( g_macro_hwnd, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>( g_macro_prev ) );
		g_macro_hwnd = nullptr;
		g_macro_prev = nullptr;
	}

	void drain_raw_input( const HRAWINPUT handle, const UINT command,
		const UINT header_size )
	{
		if ( !g_real_get_raw_input_data )
			return;
		RAWINPUT raw{};
		UINT raw_size = static_cast<UINT>( sizeof( raw ) );
		g_raw_input_detour.lift( );
		const auto got = g_real_get_raw_input_data(
			handle, command, &raw, &raw_size, header_size );
		g_raw_input_detour.rearm( );

		// Drive the cursor from the delta the game is not allowed to see.
		if ( got != 0 && got != static_cast<UINT>( -1 ) )
		{
			if ( raw.header.dwType == RIM_TYPEMOUSE )
				note_raw_mouse( raw.data.mouse );
			if ( raw.header.dwType == RIM_TYPEMOUSE
				&& ( raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE ) == 0 )
			{
				apply_mouse_delta( raw.data.mouse.lLastX, raw.data.mouse.lLastY );
			}
		}
	}

	UINT WINAPI hooked_get_raw_input_data( const HRAWINPUT handle, const UINT command,
		LPVOID data, PUINT size, const UINT header_size )
	{
		if ( !g_capture_input.load( std::memory_order_relaxed ) || data == nullptr )
		{
			g_raw_input_detour.lift( );
			const UINT result = g_real_get_raw_input_data
				? g_real_get_raw_input_data( handle, command, data, size, header_size ) : 0;
			g_raw_input_detour.rearm( );
			if ( result != 0 && result != static_cast<UINT>( -1 )
				&& data != nullptr && command == RID_INPUT )
			{
				silence_raw_packet( static_cast<RAWINPUT*>( data ) );
			}
			return result;
		}
		drain_raw_input( handle, command, header_size );
		return 0;
	}

	UINT WINAPI hooked_get_raw_input_buffer( PRAWINPUT data, PUINT size,
		const UINT header_size )
	{
		if ( !g_capture_input.load( std::memory_order_relaxed ) || data == nullptr )
		{
			g_raw_input_buffer_detour.lift( );
			const UINT result = g_real_get_raw_input_buffer
				? g_real_get_raw_input_buffer( data, size, header_size ) : 0;
			g_raw_input_buffer_detour.rearm( );
			if ( data != nullptr )
				walk_raw_blocks( data, result, true );
			return result;
		}
		// Still consume whatever is queued; the caller just never sees it. The whole
		// block list is walked: several coalesced mouse packets are routinely pending
		// between two frames, and only the first one used to move the menu cursor.
		std::uint8_t scratch[ 4096 ]{};
		UINT scratch_size = static_cast<UINT>( sizeof( scratch ) );
		g_raw_input_buffer_detour.lift( );
		const UINT blocks = g_real_get_raw_input_buffer
			? g_real_get_raw_input_buffer( reinterpret_cast<PRAWINPUT>( scratch ),
				&scratch_size, header_size )
			: 0u;
		g_raw_input_buffer_detour.rearm( );

		LONG delta_x = 0;
		LONG delta_y = 0;
		auto* block = reinterpret_cast<PRAWINPUT>( scratch );
		// NEXTRAWINPUTBLOCK is unusable here: this header set resolves it through
		// sizeof(QWORD), which is not declared under WIN32_LEAN_AND_MEAN. The
		// documented layout is simply 8-byte aligned blocks.
		constexpr std::uintptr_t k_raw_input_alignment = 8;
		for ( UINT index = 0; block && index < blocks; ++index )
		{
			if ( block->header.dwType == RIM_TYPEMOUSE )
			{
				note_raw_mouse( block->data.mouse );
				if ( ( block->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE ) == 0 )
				{
					delta_x += block->data.mouse.lLastX;
					delta_y += block->data.mouse.lLastY;
				}
			}
			const auto next = reinterpret_cast<std::uintptr_t>( block )
				+ block->header.dwSize;
			block = reinterpret_cast<PRAWINPUT>(
				( next + k_raw_input_alignment - 1 )
					& ~( k_raw_input_alignment - 1 ) );
		}
		if ( delta_x || delta_y )
			apply_mouse_delta( delta_x, delta_y );

		if ( size )
			*size = 0;
		return 0;
	}

	BOOL WINAPI hooked_set_cursor_pos( const int x, const int y )
	{
		// In mouse look the game re-centres the cursor every frame; while the menu
		// owns the mouse, its own position must win.
		if ( g_capture_input.load( std::memory_order_relaxed ) )
			return TRUE;
		if ( !g_real_set_cursor_pos )
			return FALSE;
		g_set_cursor_pos_detour.lift( );
		const BOOL result = g_real_set_cursor_pos( x, y );
		g_set_cursor_pos_detour.rearm( );
		return result;
	}

	BOOL WINAPI hooked_clip_cursor( const RECT* rect )
	{
		if ( g_capture_input.load( std::memory_order_relaxed ) )
		{
			// A release request must still go through: swallowing ClipCursor(NULL)
			// left the game's rectangle in force for as long as the menu was up.
			if ( rect == nullptr )
			{
				g_game_clip_valid = false;
				if ( !g_real_clip_cursor )
					return TRUE;
				g_clip_cursor_detour.lift( );
				const BOOL released = g_real_clip_cursor( nullptr );
				g_clip_cursor_detour.rearm( );
				return released;
			}
			// Remember what the game wanted so it can be handed back on release.
			g_game_clip_rect = *rect;
			g_game_clip_valid = true;
			return TRUE;
		}
		if ( !g_real_clip_cursor )
			return FALSE;
		g_clip_cursor_detour.lift( );
		const BOOL result = g_real_clip_cursor( rect );
		g_clip_cursor_detour.rearm( );
		return result;
	}

	int WINAPI hooked_show_cursor( const BOOL show )
	{
		if ( g_capture_input.load( std::memory_order_relaxed ) )
		{
			// Keep the cursor visible while the menu owns it: every call, show or
			// hide, is answered with a real ShowCursor(TRUE) so the display counter
			// cannot drop below zero, and each forced increment is recorded so
			// end_cursor_capture() balances the counter exactly.
			if ( !g_real_show_cursor )
				return 0;
			g_show_cursor_detour.lift( );
			const int result = g_real_show_cursor( TRUE );
			g_show_cursor_detour.rearm( );
			g_show_cursor_calls.fetch_add( 1, std::memory_order_relaxed );
			return result >= 0 ? result : 0;
		}
		if ( !g_real_show_cursor )
			return -1;
		g_show_cursor_detour.lift( );
		const int result = g_real_show_cursor( show );
		g_show_cursor_detour.rearm( );
		return result;
	}

	HCURSOR WINAPI hooked_set_cursor( const HCURSOR cursor )
	{
		// While the menu owns the mouse, ignore the game setting a null cursor
		// (which would hide it even with a positive display counter).
		if ( g_capture_input.load( std::memory_order_relaxed ) && cursor == nullptr )
			return nullptr;
		if ( !g_real_set_cursor )
			return nullptr;
		g_set_cursor_detour.lift( );
		const HCURSOR result = g_real_set_cursor( cursor );
		g_set_cursor_detour.rearm( );
		return result;
	}

	void install_raw_input_detours( )
	{
		const auto user32 = ::GetModuleHandleW( L"user32.dll" );
		if ( !user32 )
			return;
		if ( auto* entry = ::GetProcAddress( user32, "GetRawInputData" ) )
		{
			g_real_get_raw_input_data = reinterpret_cast<get_raw_input_data_fn>( entry );
			g_raw_input_detour.install( reinterpret_cast<void*>( entry ),
				reinterpret_cast<void*>( &hooked_get_raw_input_data ) );
		}
		if ( auto* entry = ::GetProcAddress( user32, "GetRawInputBuffer" ) )
		{
			g_real_get_raw_input_buffer = reinterpret_cast<get_raw_input_buffer_fn>( entry );
			g_raw_input_buffer_detour.install( reinterpret_cast<void*>( entry ),
				reinterpret_cast<void*>( &hooked_get_raw_input_buffer ) );
		}
		if ( auto* entry = ::GetProcAddress( user32, "SetCursorPos" ) )
		{
			g_real_set_cursor_pos = reinterpret_cast<set_cursor_pos_fn>( entry );
			g_set_cursor_pos_detour.install( reinterpret_cast<void*>( entry ),
				reinterpret_cast<void*>( &hooked_set_cursor_pos ) );
		}
		if ( auto* entry = ::GetProcAddress( user32, "ClipCursor" ) )
		{
			g_real_clip_cursor = reinterpret_cast<clip_cursor_fn>( entry );
			g_clip_cursor_detour.install( reinterpret_cast<void*>( entry ),
				reinterpret_cast<void*>( &hooked_clip_cursor ) );
		}
		if ( auto* entry = ::GetProcAddress( user32, "ShowCursor" ) )
		{
			g_real_show_cursor = reinterpret_cast<show_cursor_fn>( entry );
			g_show_cursor_detour.install( reinterpret_cast<void*>( entry ),
				reinterpret_cast<void*>( &hooked_show_cursor ) );
		}
		if ( auto* entry = ::GetProcAddress( user32, "SetCursor" ) )
		{
			g_real_set_cursor = reinterpret_cast<set_cursor_fn>( entry );
			g_set_cursor_detour.install( reinterpret_cast<void*>( entry ),
				reinterpret_cast<void*>( &hooked_set_cursor ) );
		}
	}

	void remove_raw_input_detours( )
	{
		remove_macro_wndproc( );
		g_cursor_tracked.store( false, std::memory_order_relaxed );
		g_raw_input_detour.remove( );
		g_raw_input_buffer_detour.remove( );
		g_set_cursor_pos_detour.remove( );
		g_clip_cursor_detour.remove( );
		g_show_cursor_detour.remove( );
		g_set_cursor_detour.remove( );
	}

	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	HWND g_window = nullptr;
	std::uint32_t g_width = 0;
	std::uint32_t g_height = 0;
	nl::Menu* g_menu = nullptr;
	// Render thread only: true while the loading window is up.
	bool g_booting = true;
	input_router g_input{};

	// ---------------------------------------------------------------- toggle
	bool key_pressed( const int virtual_key, std::atomic<bool>& previous )
	{
		const bool down = ( ::GetAsyncKeyState( virtual_key ) & 0x8000 ) != 0;
		const bool was_down = previous.exchange( down, std::memory_order_acq_rel );
		return down && !was_down;
	}

	// The swap chain's OutputWindow can be a child of the real game window and the
	// Steam overlay can own the foreground HWND for a frame. Comparing raw handles
	// dropped the focus test in those cases, which silently disabled every input
	// suppression below and left the game holding the mouse.
	bool game_focused_now()
	{
		if ( !g_window )
			return false;
		const HWND foreground = ::GetForegroundWindow();
		if ( !foreground )
			return false;
		if ( foreground != g_window
			&& ::GetAncestor( g_window, GA_ROOT ) != ::GetAncestor( foreground, GA_ROOT ) )
			return false;
		DWORD owner = 0;
		::GetWindowThreadProcessId( foreground, &owner );
		return owner == ::GetCurrentProcessId();
	}

	// ------------------------------------------------------------------- init
	void initialize_in_game( IDXGISwapChain* swap_chain )
	{
		DXGI_SWAP_CHAIN_DESC description{};
		if ( FAILED( swap_chain->GetDesc( &description ) ) )
			return;
		g_window = description.OutputWindow;
		install_macro_wndproc( g_window );

		if ( FAILED( swap_chain->GetDevice( __uuidof( ID3D11Device ),
			reinterpret_cast<void**>( &g_device ) ) ) )
			return;
		g_device->GetImmediateContext( &g_context );

		g_width = description.BufferDesc.Width;
		g_height = description.BufferDesc.Height;
		if ( g_width == 0 || g_height == 0 )
		{
			RECT client{};
			if ( g_window && ::GetClientRect( g_window, &client ) )
			{
				g_width = static_cast<std::uint32_t>( client.right - client.left );
				g_height = static_cast<std::uint32_t>( client.bottom - client.top );
			}
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		nl::apply_imgui_style();
		nl::load_fonts();
		ImGui_ImplWin32_Init( g_window );
		ImGui_ImplDX11_Init( g_device, g_context );
		// The frosted-glass backdrops of the menu windows are host-provided: without
		// this the popups, pills and the search backdrop fall back to an opaque fill
		// that hides the menu behind them instead of blurring it.
		if ( nl::blur_dx11::init( g_device, g_context ) )
			log_event( "blur_ready" );
		else
			log_event( "blur_unavailable" );
		// Borrowed by the ESP model preview; released again in DllMain before the
		// device and the ImGui context go away.
		nl::esp_model::set_device( g_device, g_context );
		// The Inventory's model inspector drives its own preview instance on the same
		// borrowed device.
		nl::model_preview::set_device( g_device, g_context );
		// Override Sky unwraps the sky cube maps into its list previews on the same device.
		nl::skies::set_device( g_device, g_context );

		const float dpi_scale = g_window
			? static_cast<float>( ::GetDpiForWindow( g_window ) ) / 96.0f : 1.0f;
		hook_hud::initialize( g_device, g_context, g_width, g_height, dpi_scale );

		g_menu = new nl::Menu();
		const float scale = std::clamp( 1.5f * dpi_scale, nl::Menu::min_scale, nl::Menu::max_scale );
		g_menu->init( ImVec2( std::floor( ( static_cast<float>( g_width )
				- std::round( nl::style::layout::width * scale ) ) * 0.5f ),
			std::floor( ( static_cast<float>( g_height )
				- std::round( nl::style::layout::height * scale ) ) * 0.5f ) ), scale );
		// Hidden until the preload is through; the loading window takes its place.
		g_menu->set_visible( false );

		static_cast<void>( g_input.initialize() );
		game::camera().set_presentation_horizon( 0.0f );
		menu_t::set_open( false );
		nl_cheat::start_internal( g_module );
		log_event( "in_game_ready" );
		g_ready.store( true, std::memory_order_release );
	}

	// ------------------------------------------------------------------ frame
	void render_frame( IDXGISwapChain* swap_chain )
	{
		const std::uint64_t frame = g_frame_index.fetch_add( 1,
			std::memory_order_acq_rel ) + 1;
		log_frame_stage( frame, "enter" );

		ID3D11Texture2D* back_buffer = nullptr;
		if ( FAILED( swap_chain->GetBuffer( 0, __uuidof( ID3D11Texture2D ),
			reinterpret_cast<void**>( &back_buffer ) ) ) )
			return;

		D3D11_TEXTURE2D_DESC description{};
		back_buffer->GetDesc( &description );
		g_width = description.Width;
		g_height = description.Height;

		ID3D11RenderTargetView* rtv = nullptr;
		const auto created = SUCCEEDED( g_device->CreateRenderTargetView(
			back_buffer, nullptr, &rtv ) );
		back_buffer->Release();
		if ( !created )
			return;
		log_frame_stage( frame, "rtv_ready" );

		if ( key_pressed( VK_INSERT, g_insert_down ) && g_menu && !g_booting )
			g_menu->toggle_visible();
		if ( key_pressed( platform::windows::lifecycle_keys().exit, g_exit_down ) )
			g_suppressed.store( !g_suppressed.load( std::memory_order_acquire ),
				std::memory_order_release );

		const bool suppressed = g_suppressed.load( std::memory_order_acquire );
		const bool menu_open = !suppressed && g_menu && g_menu->visible();
		menu_t::set_open( menu_open );

		const bool game_focused = game_focused_now();
		app::context().overlay.set_combat_input_ready(
			game_focused && !menu_open && !suppressed );

		// While the menu is up the low-level hooks consume the input so the game
		// does not see it; released again the moment the menu closes. Movement
		// keys stay with the game (see passthrough_key), mouse look does not: the
		// raw-input detours swallow it so the camera holds still. Cursor handling:
		// in a live match the game uses RIDEV_NOLEGACY (Windows stops moving the OS
		// cursor at all), so we drive it ourselves from the swallowed raw deltas.
		const bool capture = menu_open && game_focused;
		static bool capture_was_active = false;
		if ( capture != capture_was_active )
		{
			if ( capture )
				begin_cursor_capture( );
			else
				end_cursor_capture( );
			capture_was_active = capture;
		}
		g_input.set_capture( capture, capture ? g_window : nullptr );
		g_capture_input.store( capture, std::memory_order_relaxed );

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrameCached( static_cast<float>( g_width ),
			static_cast<float>( g_height ) );
		// Pumped on every frame, not only while capturing: the binds and the menu
		// shortcuts read the ImGui key state, and they have to keep working with the
		// menu closed. The router itself decides what to swallow.
		// ImGui lays the menu out in back-buffer pixels, the cursor comes in screen
		// pixels: a windowed game is offset by its frame and a stretched resolution
		// is scaled, and either one moves every hitbox away from the cursor.
		if ( g_input.capture_ready() || capture )
		{
			POINT origin{};
			RECT client{};
			float scale_x = 1.0f, scale_y = 1.0f;
			if ( g_window && ::ClientToScreen( g_window, &origin )
				&& ::GetClientRect( g_window, &client )
				&& client.right > 0 && client.bottom > 0 )
			{
				scale_x = static_cast<float>( g_width ) / static_cast<float>( client.right );
				scale_y = static_cast<float>( g_height ) / static_cast<float>( client.bottom );
			}
			g_input.pump_imgui( origin.x, origin.y, capture, scale_x, scale_y );
		}
		ImGui::GetIO().MouseDrawCursor = capture;

		// Recreated only when the back buffer size changed; the capture callback runs
		// later, inside ImGui_ImplDX11_RenderDrawData, where the game's back buffer is
		// the bound render target.
		nl::blur::set_backend( nl::blur_dx11::prepare(
			static_cast<int>( g_width ), static_cast<int>( g_height ) ) );

		ImGui::NewFrame();
		log_frame_stage( frame, "new_frame" );
		// Always ticked: the NL watermark and the bind list live outside the menu
		// visibility state.
		if ( g_menu && g_booting )
		{
			// Loading window first; the menu fades in once it has gone.
			const auto boot = nl_cheat::preload::pump( g_menu->config() );
			if ( nl::boot::loading( boot.progress, boot.text, boot.done ) )
			{
				g_booting = false;
				g_menu->set_visible( true );
				nl::boot::notify( "Successfully injected!" );
				log_event( "boot_done" );
			}
		}
		else if ( g_menu && !suppressed )
			g_menu->render();
		if ( !suppressed )
			nl::boot::notifications();
		if ( !suppressed )
			hook_hud::render_frame( rtv, g_width, g_height, menu_open );
		if ( g_menu )
			nl_cheat::sync_config( g_menu->config() );
		ImGui::Render();
		log_frame_stage( frame, "rendered" );

		// The ImGui DX11 backend backs up shaders, blend, depth-stencil and
		// viewports but NOT the output-merger render targets. Binding ours and
		// releasing it right after the draw leaves the pipeline pointing at a
		// freed view: the game then presents with a dangling RTV and the NVIDIA
		// driver faults (nvwgf2umx, 0xc0000005). Save the game's targets, draw,
		// put them back, and only then drop our reference.
		ID3D11RenderTargetView* previous_rtv = nullptr;
		ID3D11DepthStencilView* previous_dsv = nullptr;
		g_context->OMGetRenderTargets( 1, &previous_rtv, &previous_dsv );

		g_context->OMSetRenderTargets( 1, &rtv, nullptr );
		ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );

		if ( !suppressed && chams::g_renderer.ready() )
		{
			// 2D glow drawn on top of the UI, into its own targets and then
			// blended over; it does not touch the game's pipeline state.
			chams::g_renderer.render_2d_bloom( rtv, g_width, g_height );
			g_context->OMSetRenderTargets( 1, &rtv, nullptr );
		}

		g_context->OMSetRenderTargets( 1, &previous_rtv, previous_dsv );

		if ( previous_rtv ) previous_rtv->Release();
		if ( previous_dsv ) previous_dsv->Release();
		rtv->Release();
		log_frame_stage( frame, "presenting" );
	}

	HRESULT STDMETHODCALLTYPE hooked_present( IDXGISwapChain* swap_chain,
		const UINT sync_interval, const UINT flags )
	{
		// Re-entrancy here would mean something chained back into us; the
		// genuine Present is then the only safe destination.
		static thread_local bool inside = false;
		if ( inside )
			return call_original_present( swap_chain, sync_interval, flags );

		inside = true;
		ID3D11Texture2D* probe = nullptr;
		const auto probed = SUCCEEDED( swap_chain->GetBuffer( 0,
			__uuidof( ID3D11Texture2D ), reinterpret_cast<void**>( &probe ) ) );
		bool usable = false;
		if ( probed )
		{
			D3D11_TEXTURE2D_DESC description{};
			probe->GetDesc( &description );
			// Ignore helper swap chains; the game surface is a real frame.
			usable = description.Width >= 640 && description.Height >= 360;
			probe->Release();
		}

		if ( usable )
		{
			if ( !g_ready.load( std::memory_order_acquire ) )
			{
				static std::atomic<bool> initializing{ false };
				bool expected = false;
				if ( initializing.compare_exchange_strong( expected, true,
					std::memory_order_acq_rel ) )
					initialize_in_game( swap_chain );
			}
			if ( g_ready.load( std::memory_order_acquire ) )
				render_frame( swap_chain );
		}

		const HRESULT result = call_original_present( swap_chain, sync_interval, flags );
		inside = false;
		return result;
	}

	bool install_hook()
	{
		// A throwaway swap chain exposes the shared IDXGISwapChain vtable.
		WNDCLASSEXW window_class{};
		window_class.cbSize = sizeof( window_class );
		window_class.lpfnWndProc = ::DefWindowProcW;
		window_class.hInstance = g_module;
		window_class.lpszClassName = L"neverlose_hook_bootstrap";
		::RegisterClassExW( &window_class );
		const HWND window = ::CreateWindowExW( 0, window_class.lpszClassName,
			L"neverlose", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, g_module, nullptr );
		if ( !window )
			return false;

		DXGI_SWAP_CHAIN_DESC description{};
		description.BufferCount = 1;
		description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.OutputWindow = window;
		description.SampleDesc.Count = 1;
		description.Windowed = TRUE;
		description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		IDXGISwapChain* swap_chain = nullptr;
		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL level{};
		const auto created = ::D3D11CreateDeviceAndSwapChain( nullptr,
			D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
			&description, &swap_chain, &device, &level, &context );
		if ( FAILED( created ) || !swap_chain )
		{
			::DestroyWindow( window );
			::UnregisterClassW( window_class.lpszClassName, g_module );
			return false;
		}

		auto** vtable = *reinterpret_cast<void***>( swap_chain );
		const auto slot_entry = reinterpret_cast<std::uintptr_t>(
			vtable[ k_vtable_present_index ] );
		g_real_present = resolve_real_present( slot_entry );

		swap_chain->Release();
		if ( context ) context->Release();
		if ( device ) device->Release();
		::DestroyWindow( window );
		::UnregisterClassW( window_class.lpszClassName, g_module );

		if ( !g_real_present )
		{
			log_event( "present_hook_no_genuine_present" );
			return false;
		}

		// Only the genuine code is touched; every vtable stays as the game and
		// the other overlays left it.
		if ( !install_present_detour( g_real_present ) )
			return false;

		g_installed.store( true, std::memory_order_release );
		return true;
	}

	void bootstrap()
	{
		// The loader lock is released as soon as DllMain returns; give it a beat
		// before touching D3D.
		std::this_thread::sleep_for( std::chrono::milliseconds( 120 ) );
		install_raw_input_detours( );
		log_event( install_hook() ? "present_hook_installed" : "present_hook_failed" );
	}

} // namespace

BOOL WINAPI DllMain( HINSTANCE instance, const DWORD reason, LPVOID reserved )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		g_module = instance;
		::DisableThreadLibraryCalls( instance );
		// First in the chain, so the fault is written down before anything else runs.
		::AddVectoredExceptionHandler( 1, crash_filter );
		log_event( "dllmain_attach" );
		std::thread( bootstrap ).detach();
	}
	else if ( reason == DLL_PROCESS_DETACH && reserved == nullptr )
	{
		log_event( "dllmain_detach" );
		// Hand the genuine code back before this image disappears.
		g_capture_input.store( false, std::memory_order_relaxed );
		remove_raw_input_detours( );
		remove_present_detour( );
		nl_cheat::shutdown();
		// The preview holds the game's device and an SRV the menu may still have in
		// its draw list; drop it before the context and device are released.
		nl::model_preview::release();
		nl::esp_model::release();
		nl::skies::release();
		nl::blur::set_backend( {} );
		nl::blur_dx11::shutdown();
		g_input.shutdown();
		if ( g_context ) g_context->Release();
		if ( g_device ) g_device->Release();
	}
	return TRUE;
}
