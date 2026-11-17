#include <port/cheat_bridge.hpp>

#include <atomic>
#include <array>
#include <filesystem>
#include <thread>

#include <app/context.hpp>
#include <app/workers.hpp>
#include <config/settings.hpp>
#include <core/input/input.hpp>
#include <core/memory/modules.hpp>
#include <core/memory/process.hpp>
#include <port/probe.hpp>
#include <features/visuals/avatars.hpp>
#include <render/menu/menu.hpp>
#include <scripting/runtime.hpp>

namespace nl_cheat {
namespace {

	std::atomic<bool> g_run{ false };
	std::atomic<bool> g_internal{ false };
	std::jthread g_engine{};
	HMODULE g_module{ nullptr };

	void release_game_input()
	{
		auto& input = app::context().input;
		input.set_key_gate( 0, false );
		input.set_movement_gate( {}, false );
		input.key( VK_ESCAPE, false );
		const std::array movement_releases{
			platform::windows::input_gateway::key_transition{ VK_CONTROL, false },
			platform::windows::input_gateway::key_transition{ VK_F24, false },
		};
		input.keys( movement_releases );
		input.pointer(
			0, 0,
			platform::windows::pointer_action::primary_up
				| platform::windows::pointer_action::secondary_up );
	}

	// The scripts shipped next to the module (exe folder externally, the DLL
	// folder when injected into the game).
	void seed_scripts()
	{
		const auto runtime_scripts =
			platform::windows::runtime_storage::area( "lua" ) / L"scripts";
		std::error_code error{};
		std::filesystem::create_directories( runtime_scripts, error );
		if ( error )
			return;

		wchar_t module_path[ MAX_PATH ]{};
		if ( !::GetModuleFileNameW( g_module, module_path,
			static_cast<DWORD>( std::size( module_path ) ) ) )
			return;

		const auto shipped = std::filesystem::path( module_path )
			.parent_path() / L"lua" / L"scripts";
		if ( !std::filesystem::is_directory( shipped, error ) )
			return;

		std::filesystem::copy( shipped, runtime_scripts,
			std::filesystem::copy_options::recursive
				| std::filesystem::copy_options::overwrite_existing,
			error );
	}

	void engine_main()
	{
		auto& ctx = app::context();
		const bool internal = g_internal.load( std::memory_order_acquire );

		// No console in the menu build; open() is a no-op returning true.
		static_cast<void>( ctx.diagnostics.open( "Neverlose :> " ) );

		bool workers_started = false;
		bool scripting_started = false;

		// Attach loop: externally the menu stays usable while cs2 is not running
		// yet; internally the host process is already cs2, so this succeeds at once.
		while ( g_run.load( std::memory_order_acquire ) )
		{
			if ( !ctx.process.attach( L"cs2.exe" )
				|| !ctx.input.connect()
				|| !ctx.modules.discover( ctx.process )
				|| !ctx.addresses.initialize()
				|| !game::fields().initialize() )
			{
				for ( int i = 0; i < 30 && g_run.load( std::memory_order_acquire ); ++i )
					std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
				continue;
			}

			if ( !scripting_started )
			{
				config::apply_default_config();
				if ( !config::storage.read_cache() )
					config::storage.write_cache();
				seed_scripts();
				scripting_started = scripting::runtime().initialize();
				if ( !scripting_started )
					config::general_settings.lua_enabled = false;
			}
			config::publish_runtime_snapshot();

			// Schema probe: runs once when %TEMP%\vesta\probe.on exists, then consumes
			// the marker and writes probe_schema.txt. It is the only way to learn which
			// field names the live CS2 schema actually carries.
			run_probe_if_armed( );

			// Steam avatars for the ESP nickname: the worker fetches profiles on demand.
			features::visuals::avatars( ).start( );

			if ( !internal )
			{
				// Stop sending input the moment the game dies, and hide the overlay.
				const auto game_process = static_cast<HANDLE>(
					ctx.process.native_handle() );
				std::thread( [ game_process ]
				{
					if ( ::WaitForSingleObject( game_process, INFINITE ) != WAIT_OBJECT_0 )
						return;
					::ClipCursor( nullptr );
					release_game_input();
					app::context().overlay.request_shutdown();
				} ).detach();
			}

			if ( !workers_started )
			{
				workers_started = true;
				std::thread( app::workers::game ).detach();
				std::thread( app::workers::movement ).detach();
				std::thread( app::workers::combat ).detach();
				std::thread( app::workers::nade_helper ).detach();
				std::thread( app::workers::seed_trigger ).detach();
			}

			if ( internal )
			{
				// The present hook owns presentation; stay resident until unload.
				while ( g_run.load( std::memory_order_acquire ) )
				{
					std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
				}
				break;
			}

			// External build: the vesta overlay is NOT launched. This process has a
			// single ImGui context (the Neverlose menu); a second context on another
			// thread would race on ImGui globals. Combat gating follows the game's
			// foreground window instead.
			while ( g_run.load( std::memory_order_acquire ) )
			{
				const HWND foreground = ::GetForegroundWindow();
				DWORD owner = 0;
				if ( foreground )
					::GetWindowThreadProcessId( foreground, &owner );
				app::context().overlay.set_combat_input_ready(
					owner != 0 && owner == ctx.process.process_id()
						&& !app::context().menu.is_open() );
				std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
			}
			app::context().overlay.set_combat_input_ready( false );
			break;
		}

		release_game_input();
		scripting::runtime().shutdown();
	}

} // namespace

void set_menu_hwnd( HWND hwnd ) noexcept
{
	menu_t::set_nl_window( hwnd );
}

void start()
{
	if ( g_engine.joinable() )
		return;
	g_internal.store( false, std::memory_order_release );
	g_module = ::GetModuleHandleW( nullptr );
	g_run.store( true, std::memory_order_release );
	g_engine = std::jthread( engine_main );
}

void start_internal( void* module )
{
	if ( g_engine.joinable() )
		return;
	g_internal.store( true, std::memory_order_release );
	g_module = static_cast<HMODULE>( module );
	g_run.store( true, std::memory_order_release );
	g_engine = std::jthread( engine_main );
}

bool running() noexcept
{
	return g_run.load( std::memory_order_acquire );
}

void shutdown() noexcept
{
	g_run.store( false, std::memory_order_release );
	if ( !g_internal.load( std::memory_order_acquire ) )
		app::context().overlay.request_shutdown();
	if ( g_engine.joinable() )
		g_engine.join();
	g_engine = {};
}

} // namespace nl_cheat
