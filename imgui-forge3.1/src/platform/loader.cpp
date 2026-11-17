// Loader: injects neverlose_menu.dll into cs2.exe. The window lives in
// main_loader_dx11.cpp; this file is the injection job. The DLL draws the cheat
// inside the game frame.
//
// Every step of the way is logged to <TEMP>\vesta\loader.log and mirrored on the
// console. Failures are diagnosed instead of guessed: a null LoadLibraryW result
// is turned into the real reason (missing import, blocked write, architecture
// mismatch) rather than a generic "injection failed".

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <tlhelp32.h>

// -municode maps ShellExecute to ShellExecuteW. Undefined before the shell
// interface headers, otherwise the IShellDispatch2 method method is declared
// under the W name and the call below does not match it.
#ifdef ShellExecute
#undef ShellExecute
#endif
#include <exdisp.h>
#include <servprov.h>
#include <shldisp.h>
#include <shlobj.h>

#include "platform/inject.hpp"

namespace {

	// ------------------------------------------------------------ reporting
	std::ofstream g_log;

	struct Feedback
	{
		nl::InjectReportFn fn = nullptr;
		void* user = nullptr;
		std::atomic<bool>* cancel = nullptr;
	};

	Feedback g_feedback;
	int g_fail_stage = static_cast<int>( nl::InjectStage::InjectFailed );

	void ui_note( const float progress, const int stage, const bool failed = false, const bool done = false )
	{
		if ( g_feedback.fn )
			g_feedback.fn( g_feedback.user, progress, stage, failed, done );
	}

	[[nodiscard]] bool ui_cancel()
	{
		return g_feedback.cancel && g_feedback.cancel->load( std::memory_order_relaxed );
	}

	void open_log()
	{
		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return;
		const auto directory = std::filesystem::path( temporary ) / L"vesta";
		std::error_code error{};
		std::filesystem::create_directories( directory, error );
		if ( g_log.is_open() )
			g_log.close();
		g_log.open( directory / L"loader.log", std::ios::trunc );
	}

	void log_line( const char* format, ... )
	{
		char text[ 1024 ]{};
		va_list arguments;
		va_start( arguments, format );
		const int count = std::vsnprintf( text, sizeof( text ), format, arguments );
		va_end( arguments );
		if ( count <= 0 )
			return;

		std::fputs( text, stdout );
		std::fflush( stdout );
		if ( g_log )
		{
			g_log << "tick=" << ::GetTickCount64( ) << " " << text;
			g_log.flush( );
		}
	}

	// A vanishing console is the reason "the loader does nothing" is such a
	// common report: the window closes before the message is read. Anything
	// fatal stays on screen until a key is pressed.
	void hold_console()
	{
		if ( !::GetConsoleWindow() )
			return;
		std::fputs( "\npress Enter to close...", stdout );
		std::fflush( stdout );
		std::getchar( );
	}

	[[nodiscard]] const char* describe_error( const DWORD code )
	{
		switch ( code )
		{
		case ERROR_ACCESS_DENIED: return "access denied";
		case ERROR_INVALID_PARAMETER: return "no such process or invalid parameter";
		case ERROR_PARTIAL_COPY: return "partial copy -- wrong architecture (bitness)";
		case ERROR_NOT_ENOUGH_MEMORY: return "not enough memory in the target";
		case ERROR_DLL_NOT_FOUND: return "a dependency of the module is missing";
		case ERROR_MOD_NOT_FOUND: return "module or one of its dependencies not found";
		case ERROR_BAD_EXE_FORMAT: return "bad image format for that target";
		case ERROR_DLL_INIT_FAILED: return "DllMain returned FALSE (module refused to load)";
		case ERROR_INVALID_HANDLE: return "invalid handle";
		case ERROR_GEN_FAILURE: return "device attached to the system is not functioning";
		default: return "";
		}
	}

	void log_error( const char* what, const DWORD code )
	{
		const char* text = describe_error( code );
		if ( *text )
			log_line( "%s failed: %s (error %lu)\n", what, text, code );
		else
			log_line( "%s failed (error %lu)\n", what, code );
	}

	// ----------------------------------------------------------------- utils
	template <class T>
	void release( T*& object )
	{
		if ( object )
			object->Release( );
		object = nullptr;
	}

	[[nodiscard]] bool is_admin()
	{
		BOOL member = FALSE;
		SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
		PSID administrators = nullptr;
		if ( !::AllocateAndInitializeSid( &authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
			DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators ) )
			return false;
		const BOOL ok = ::CheckTokenMembership( nullptr, administrators, &member );
		::FreeSid( administrators );
		return ok != FALSE && member != FALSE;
	}

	// The loader runs elevated (OpenProcess on cs2 needs it). Opening a steam://
	// URL from here starts an elevated steam.exe that cannot reach the user's
	// running client ("registry key not writable"), and spawning explorer.exe from
	// an elevated process can fail to start at all (0xc0000142). Instead the
	// desktop's own Explorer is asked to open the URL, so it runs with the user's
	// normal token and lands in the client that is already running.
	[[nodiscard]] bool shell_execute_unelevated( const wchar_t* target )
	{
		// SID_STopLevelBrowser, spelled out so no extra GUID library is needed.
		static constexpr GUID k_top_level_browser{ 0x4C96BE40, 0x915C, 0x11CF,
			{ 0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37 } };

		const HRESULT com = ::CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
		IShellWindows* windows = nullptr;
		IDispatch* desktop = nullptr;
		IServiceProvider* provider = nullptr;
		IShellBrowser* browser = nullptr;
		IShellView* view = nullptr;
		IDispatch* background = nullptr;
		IShellFolderViewDual* folder = nullptr;
		IDispatch* application = nullptr;
		IShellDispatch2* shell = nullptr;

		HRESULT hr = ::CoCreateInstance( CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
			IID_IShellWindows, reinterpret_cast<void**>( &windows ) );
		VARIANT none;
		::VariantInit( &none );
		long window = 0;
		if ( SUCCEEDED( hr ) )
			hr = windows->FindWindowSW( &none, &none, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &desktop );
		if ( SUCCEEDED( hr ) && !desktop )
			hr = E_FAIL; // S_FALSE: no desktop window (another shell is running)
		if ( SUCCEEDED( hr ) )
			hr = desktop->QueryInterface( IID_IServiceProvider, reinterpret_cast<void**>( &provider ) );
		if ( SUCCEEDED( hr ) )
			hr = provider->QueryService( k_top_level_browser, IID_IShellBrowser,
				reinterpret_cast<void**>( &browser ) );
		if ( SUCCEEDED( hr ) )
			hr = browser->QueryActiveShellView( &view );
		if ( SUCCEEDED( hr ) )
			hr = view->GetItemObject( SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>( &background ) );
		if ( SUCCEEDED( hr ) )
			hr = background->QueryInterface( IID_IShellFolderViewDual, reinterpret_cast<void**>( &folder ) );
		if ( SUCCEEDED( hr ) )
			hr = folder->get_Application( &application );
		if ( SUCCEEDED( hr ) )
			hr = application->QueryInterface( IID_IShellDispatch2, reinterpret_cast<void**>( &shell ) );
		if ( SUCCEEDED( hr ) )
		{
			BSTR file = ::SysAllocString( target );
			VARIANT show;
			::VariantInit( &show );
			show.vt = VT_I4;
			show.lVal = SW_SHOWNORMAL;
			hr = file ? shell->ShellExecute( file, none, none, none, show ) : E_OUTOFMEMORY;
			::SysFreeString( file );
		}

		release( shell );
		release( application );
		release( folder );
		release( background );
		release( view );
		release( browser );
		release( provider );
		release( desktop );
		release( windows );
		if ( SUCCEEDED( com ) )
			::CoUninitialize( );
		return SUCCEEDED( hr );
	}

	[[nodiscard]] DWORD find_process( const wchar_t* executable )
	{
		const HANDLE snapshot = ::CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
		if ( snapshot == INVALID_HANDLE_VALUE )
			return 0;

		PROCESSENTRY32W entry{ .dwSize = sizeof( entry ) };
		DWORD found = 0;
		if ( ::Process32FirstW( snapshot, &entry ) )
		{
			do
			{
				if ( ::CompareStringOrdinal( entry.szExeFile, -1, executable, -1, TRUE )
					== CSTR_EQUAL )
				{
					found = entry.th32ProcessID;
					break;
				}
			}
			while ( ::Process32NextW( snapshot, &entry ) );
		}
		::CloseHandle( snapshot );
		return found;
	}

	[[nodiscard]] bool process_alive( const DWORD process_id )
	{
		if ( !process_id )
			return false;
		const HANDLE process = ::OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id );
		if ( !process )
			return false;
		DWORD code = 0;
		const bool alive = ::GetExitCodeProcess( process, &code ) && code == STILL_ACTIVE;
		::CloseHandle( process );
		return alive;
	}

	// Distinguishes "the game is elevated and we are not" from a plain refusal,
	// which is the difference between one error message and a useful one. Needs
	// only PROCESS_QUERY_LIMITED_INFORMATION, so it still works on processes that
	// reject the write access OpenProcess would ask for.
	[[nodiscard]] bool process_is_elevated( const DWORD process_id )
	{
		const HANDLE process = ::OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id );
		if ( !process )
			return false;
		HANDLE token = nullptr;
		bool elevated = false;
		if ( ::OpenProcessToken( process, TOKEN_QUERY, &token ) )
		{
			TOKEN_ELEVATION elevation{};
			DWORD size = sizeof( elevation );
			if ( ::GetTokenInformation( token, TokenElevation, &elevation, size, &size ) )
				elevated = elevation.TokenIsElevated != 0;
			::CloseHandle( token );
		}
		::CloseHandle( process );
		return elevated;
	}

	[[nodiscard]] std::uintptr_t find_module( const DWORD process_id,
		const wchar_t* name )
	{
		const HANDLE snapshot = ::CreateToolhelp32Snapshot(
			TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id );
		if ( snapshot == INVALID_HANDLE_VALUE )
			return 0;

		MODULEENTRY32W entry{ .dwSize = sizeof( entry ) };
		std::uintptr_t base = 0;
		if ( ::Module32FirstW( snapshot, &entry ) )
		{
			do
			{
				if ( ::CompareStringOrdinal( entry.szModule, -1, name, -1, TRUE )
					== CSTR_EQUAL )
				{
					base = reinterpret_cast<std::uintptr_t>( entry.modBaseAddr );
					break;
				}
			}
			while ( ::Module32NextW( snapshot, &entry ) );
		}
		::CloseHandle( snapshot );
		return base;
	}

	// ------------------------------------------------------- image inspection
	[[nodiscard]] std::uint32_t rva_to_offset( const std::vector<std::uint8_t>& image,
		const std::uint32_t rva )
	{
		if ( image.size() < 0x40 )
			return 0;
		const auto pe = *reinterpret_cast<const std::uint32_t*>( image.data() + 0x3C );
		if ( static_cast<std::size_t>( pe ) + 0x18 > image.size() )
			return 0;
		const auto sections = *reinterpret_cast<const std::uint16_t*>( image.data() + pe + 6 );
		const auto optional_size = *reinterpret_cast<const std::uint16_t*>( image.data() + pe + 20 );
		const auto optional = static_cast<std::size_t>( pe ) + 24;
		if ( optional + optional_size > image.size() )
			return 0;

		for ( std::uint16_t index = 0; index < sections; ++index )
		{
			const auto entry = optional + optional_size + static_cast<std::size_t>( index ) * 40;
			if ( entry + 40 > image.size() )
				return 0;
			const auto virtual_size = *reinterpret_cast<const std::uint32_t*>( image.data() + entry + 8 );
			const auto virtual_address = *reinterpret_cast<const std::uint32_t*>( image.data() + entry + 12 );
			const auto raw_size = *reinterpret_cast<const std::uint32_t*>( image.data() + entry + 16 );
			const auto raw = *reinterpret_cast<const std::uint32_t*>( image.data() + entry + 20 );
			const auto span = virtual_size > raw_size ? virtual_size : raw_size;
			if ( rva >= virtual_address && rva < virtual_address + span )
				return raw + ( rva - virtual_address );
		}
		return 0;
	}

	// A local copy of the module is parsed for its import names. That turns the
	// target-side "LoadLibraryW returned null" into "cs2.exe is missing
	// d3dcompiler_47.dll", which is actionable.
	void report_dependencies( const std::wstring& library )
	{
		std::ifstream stream( std::filesystem::path( library ), std::ios::binary );
		if ( !stream )
		{
			log_line( "  (could not read the module for a dependency check)\n" );
			return;
		}
		const std::vector<std::uint8_t> image(
			( std::istreambuf_iterator<char>( stream ) ), std::istreambuf_iterator<char>( ) );

		std::uint32_t import_rva = 0;
		do
		{
			if ( image.size() < 0x40 )
				break;
			const auto pe = *reinterpret_cast<const std::uint32_t*>( image.data() + 0x3C );
			const auto optional = pe + 24;
			if ( optional + 120 > image.size() )
				break;
			const auto magic = *reinterpret_cast<const std::uint16_t*>( image.data() + optional );
			const auto directory = magic == 0x20B ? optional + 112 : optional + 96;
			if ( directory + 16 > image.size() )
				break;
			import_rva = *reinterpret_cast<const std::uint32_t*>( image.data() + directory + 8 );
		}
		while ( false );
		const auto offset = rva_to_offset( image, import_rva );
		if ( !offset )
			return;

		std::string missing;
		for ( std::size_t index = 0; index < 256; ++index )
		{
			const auto entry = offset + index * 20;
			if ( entry + 20 > image.size() )
				break;
			const auto names = *reinterpret_cast<const std::uint32_t*>( image.data() + entry + 12 );
			if ( names == 0 )
				break;
			const auto name_offset = rva_to_offset( image, names );
			if ( !name_offset || name_offset >= image.size() )
				continue;
			const char* name = reinterpret_cast<const char*>( image.data() + name_offset );
			const auto available = image.size() - name_offset;
			const auto length = ::strnlen( name, available );
			if ( length == 0 || length >= available )
				continue;
			const std::wstring wide( name, name + length );
			if ( ::GetModuleHandleW( wide.c_str() ) )
				continue;
			if ( !missing.empty() )
				missing += ", ";
			missing.append( name, length );
		}
		if ( missing.empty( ) )
			log_line( "  every imported module is present in the loader process\n" );
		else
			log_line( "  not loaded here (may still resolve in cs2.exe): %s\n", missing.c_str( ) );
	}

	// -------------------------------------------------------------- injection
	// Third-party overlays (Steam's gameoverlayrenderer64) hook the shared
	// IDXGISwapChain vtable too and chain by re-reading the slot they were
	// installed over. An internal hook that lands in front of them ping-pongs
	// with them until the stack overflows (c00000fd). Resolve the genuine
	// dxgi Present in this clean process and publish its RVA for the DLL, which
	// maps it onto the game's own dxgi.dll.
	[[nodiscard]] std::uintptr_t present_rva()
	{
		WNDCLASSEXW window_class{ .cbSize = sizeof( WNDCLASSEXW ) };
		window_class.lpfnWndProc = ::DefWindowProcW;
		window_class.hInstance = ::GetModuleHandleW( nullptr );
		window_class.lpszClassName = L"neverlose_rva_probe";
		if ( !::RegisterClassExW( &window_class ) )
			return 0;

		const HWND window = ::CreateWindowExW( 0, window_class.lpszClassName,
			L"probe", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
			window_class.hInstance, nullptr );
		if ( !window )
		{
			::UnregisterClassW( window_class.lpszClassName, window_class.hInstance );
			return 0;
		}

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
		const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL level{};
		const auto created = ::D3D11CreateDeviceAndSwapChain( nullptr,
			D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
			&description, &swap_chain, &device, &level, &context );

		std::uintptr_t offset = 0;
		if ( SUCCEEDED( created ) && swap_chain )
		{
			const auto entry = reinterpret_cast<std::uintptr_t>(
				( *reinterpret_cast<void***>( swap_chain ) )[ 8 ] );
			const auto base = reinterpret_cast<std::uintptr_t>(
				::GetModuleHandleW( L"dxgi.dll" ) );
			if ( entry > base && base != 0 && entry - base < 0x4000000 )
				offset = entry - base;
		}

		if ( context ) context->Release();
		if ( device ) device->Release();
		if ( swap_chain ) swap_chain->Release();
		::DestroyWindow( window );
		::UnregisterClassW( window_class.lpszClassName, window_class.hInstance );
		return offset;
	}

	void publish_present_rva()
	{
		const auto offset = present_rva();
		if ( offset == 0 )
		{
			log_line( "warning: the genuine Present address was not resolved;\n"
				"the DLL will refuse to hook a foreign entry.\n" );
			return;
		}

		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return;
		const auto directory = std::filesystem::path( temporary ) / L"vesta";
		std::error_code error{};
		std::filesystem::create_directories( directory, error );
		std::ofstream stream( directory / L"present_rva.txt", std::ios::trunc );
		if ( !stream )
		{
			log_line( "warning: could not publish the Present RVA to %ls\n",
				( directory / L"present_rva.txt" ).c_str() );
			return;
		}
		stream << std::hex << offset << '\n';
		log_line( "genuine Present RVA: 0x%llx\n",
			static_cast<unsigned long long>( offset ) );
	}

	// Reads a NUL-terminated export name out of the remote module.
	[[nodiscard]] bool read_remote_string( const HANDLE process,
		const std::uintptr_t address, char* out, const std::size_t capacity )
	{
		if ( !address || capacity < 2 )
			return false;
		SIZE_T read = 0;
		if ( !::ReadProcessMemory( process, reinterpret_cast<const void*>( address ),
			out, capacity - 1, &read ) )
			return false;
		out[ capacity - 1 ] = '\0';
		return read != 0;
	}

	// Resolves LoadLibraryW through the *target's* own kernel32 export directory.
	// Asking our kernel32 for the address used to be enough, but forwards,
	// hot-patching and the module list's base-DLL view can disagree with the real
	// entry; walking the remote table removes that whole class of failure. 0 means
	// kernel32 was not where the PEB says it is.
	[[nodiscard]] std::uintptr_t remote_load_library( const HANDLE process,
		const DWORD process_id, const wchar_t* module_name = L"kernel32.dll" )
	{
		const auto remote_base = find_module( process_id, module_name );
		if ( !remote_base )
			return 0;

		IMAGE_DOS_HEADER dos{};
		SIZE_T read = 0;
		if ( !::ReadProcessMemory( process, reinterpret_cast<const void*>( remote_base ),
			&dos, sizeof( dos ), &read ) || dos.e_magic != IMAGE_DOS_SIGNATURE )
			return 0;

		IMAGE_NT_HEADERS64 nt{};
		if ( !::ReadProcessMemory( process,
			reinterpret_cast<const void*>( remote_base + dos.e_lfanew ), &nt, sizeof( nt ), &read )
			|| nt.Signature != IMAGE_NT_SIGNATURE )
			return 0;

		const auto& directory = nt.OptionalHeader.DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ];
		if ( directory.VirtualAddress == 0 || directory.Size == 0 )
			return 0;

		IMAGE_EXPORT_DIRECTORY exports{};
		if ( !::ReadProcessMemory( process,
			reinterpret_cast<const void*>( remote_base + directory.VirtualAddress ),
			&exports, sizeof( exports ), &read ) )
			return 0;

		const std::uintptr_t names = remote_base + exports.AddressOfNames;
		const std::uintptr_t ordinals = remote_base + exports.AddressOfNameOrdinals;
		const std::uintptr_t functions = remote_base + exports.AddressOfFunctions;

		for ( DWORD index = 0; index < exports.NumberOfNames; ++index )
		{
			DWORD name_rva = 0;
			if ( !::ReadProcessMemory( process,
				reinterpret_cast<const void*>( names + index * sizeof( DWORD ) ),
				&name_rva, sizeof( name_rva ), &read ) )
				break;

			char name[ 64 ]{};
			if ( !read_remote_string( process, remote_base + name_rva, name, sizeof( name ) ) )
				continue;
			if ( ::strcmp( name, "LoadLibraryW" ) != 0 )
				continue;

			WORD ordinal = 0;
			if ( !::ReadProcessMemory( process,
				reinterpret_cast<const void*>( ordinals + index * sizeof( WORD ) ),
				&ordinal, sizeof( ordinal ), &read ) )
				return 0;

			DWORD function_rva = 0;
			if ( !::ReadProcessMemory( process,
				reinterpret_cast<const void*>( functions + ordinal * sizeof( DWORD ) ),
				&function_rva, sizeof( function_rva ), &read ) )
				return 0;

			// A forwarder RVA points inside the export directory; kernel32 is not
			// expected to forward LoadLibraryW, so treat it as unresolvable.
			if ( function_rva >= directory.VirtualAddress
				&& function_rva < directory.VirtualAddress + directory.Size )
				return 0;
			return remote_base + function_rva;
		}
		return 0;
	}

	// Returns the remote thread's exit code (the module base on success) or
	// false when the thread itself could not be created or did not finish.
	[[nodiscard]] bool run_remote( const HANDLE process, void* routine, void* argument,
		DWORD* remote_result )
	{
		if ( remote_result )
			*remote_result = 0;
		const HANDLE thread = ::CreateRemoteThread( process, nullptr, 0,
			reinterpret_cast<LPTHREAD_START_ROUTINE>( routine ), argument, 0, nullptr );
		if ( !thread )
		{
			log_error( "CreateRemoteThread", ::GetLastError() );
			return false;
		}

		const DWORD waited = ::WaitForSingleObject( thread, 30000 );
		DWORD exit_code = 0;
		::GetExitCodeThread( thread, &exit_code );
		::CloseHandle( thread );
		if ( remote_result )
			*remote_result = exit_code;

		if ( waited == WAIT_TIMEOUT )
		{
			log_line( "  the remote thread is still running after 30s "
				"(DllMain blocked or the module hung)\n" );
			return false;
		}
		return true;
	}

	[[nodiscard]] bool module_present( const DWORD process_id, const wchar_t* name )
	{
		for ( int attempt = 0; attempt < 20; ++attempt )
		{
			if ( find_module( process_id, name ) )
				return true;
			std::this_thread::sleep_for( std::chrono::milliseconds( 150 ) );
		}
		return false;
	}

	[[nodiscard]] bool inject( const DWORD process_id, const std::wstring& dll )
	{
		g_fail_stage = static_cast<int>( nl::InjectStage::InjectFailed );
		const HANDLE process = ::OpenProcess(
			PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
				| PROCESS_VM_WRITE | PROCESS_VM_READ,
			FALSE, process_id );
		if ( !process )
		{
			g_fail_stage = static_cast<int>( nl::InjectStage::OpenFailed );
			const DWORD error = ::GetLastError( );
			log_error( "OpenProcess", error );
			if ( error == ERROR_ACCESS_DENIED )
			{
				if ( !is_admin( ) )
					log_line( "  the loader has no admin token: right-click it and pick\n"
						"  \"Run as administrator\" (or accept the UAC prompt).\n" );
				else if ( process_is_elevated( process_id ) )
					log_line( "  cs2.exe runs elevated as well; start both from the same\n"
						"  elevation level.\n" );
				else
					log_line( "  cs2.exe refused the access. Usually this is an\n"
						"  anti-cheat-protected or already-hooked process, or a security\n"
						"  product blocking writes into another process.\n" );
			}
			return false;
		}

		const std::size_t bytes = ( dll.size( ) + 1 ) * sizeof( wchar_t );
		void* remote = ::VirtualAllocEx( process, nullptr, bytes,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
		if ( !remote )
		{
			log_error( "VirtualAllocEx", ::GetLastError() );
			::CloseHandle( process );
			return false;
		}

		bool ok = ::WriteProcessMemory( process, remote, dll.c_str(), bytes, nullptr ) != FALSE;
		if ( !ok )
			log_error( "WriteProcessMemory", ::GetLastError() );

		if ( ok )
		{
			const auto remote_entry = remote_load_library( process, process_id );
			if ( remote_entry )
				log_line( "  LoadLibraryW resolved in the target at 0x%llx\n",
					static_cast<unsigned long long>( remote_entry ) );
			else
				log_line( "  the target's kernel32 export table was not readable; "
					"using the local LoadLibraryW address\n" );

			const auto entry = remote_entry ? remote_entry
				: reinterpret_cast<std::uintptr_t>( ::GetProcAddress(
					::GetModuleHandleW( L"kernel32.dll" ), "LoadLibraryW" ) );
			if ( !entry )
			{
				log_line( "  LoadLibraryW could not be resolved at all\n" );
				ok = false;
			}
			else
			{
				DWORD result = 0;
				ok = run_remote( process, reinterpret_cast<void*>( entry ), remote, &result )
					&& result != 0;
				if ( !ok )
				{
					if ( result == 0 )
					{
						log_line( "  the module did not load. dependencies of "
							"neverlose_menu.dll:\n" );
						report_dependencies( dll );
					}
					else
						log_line( "  LoadLibraryW reported 0x%lx\n", result );
				}
			}
		}

		::VirtualFreeEx( process, remote, 0, MEM_RELEASE );
		::CloseHandle( process );
		return ok;
	}

	[[nodiscard]] bool eject( const DWORD process_id, const wchar_t* module_name )
	{
		const auto base = find_module( process_id, module_name );
		if ( !base )
		{
			log_line( "  module is not loaded\n" );
			return false;
		}

		const HANDLE process = ::OpenProcess(
			PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
				| PROCESS_VM_WRITE | PROCESS_VM_READ,
			FALSE, process_id );
		if ( !process )
		{
			log_error( "OpenProcess", ::GetLastError() );
			return false;
		}

		const auto free_library = ::GetProcAddress(
			::GetModuleHandleW( L"kernel32.dll" ), "FreeLibrary" );
		const bool ok = free_library != nullptr
			&& run_remote( process, reinterpret_cast<void*>( free_library ),
				reinterpret_cast<void*>( base ), nullptr );
		::CloseHandle( process );
		return ok;
	}

} // namespace

namespace nl {

int run_inject( const InjectOptions& options, InjectReportFn report, void* user, std::atomic<bool>* cancel )
{
	g_feedback.fn = report;
	g_feedback.user = user;
	g_feedback.cancel = cancel;

	open_log( );

	const bool unload = options.unload;
	const bool wait_for_game = options.wait_for_game || options.launch_game;
	const bool launch_game = options.launch_game;
	const bool hold = options.hold;
	const DWORD explicit_pid = options.pid;

	wchar_t loader_path[ MAX_PATH ]{};
	::GetModuleFileNameW( nullptr, loader_path,
		static_cast<DWORD>( std::size( loader_path ) ) );
	const auto directory = std::filesystem::path( loader_path ).parent_path();
	const auto library_path = ( directory / L"neverlose_menu.dll" ).wstring( );

	log_line( "loader: %ls\n", loader_path );
	log_line( "module: %ls\n", library_path.c_str( ) );
	log_line( "admin: %s\n", is_admin( ) ? "yes" : "no" );
	ui_note( 0.06f, static_cast<int>( InjectStage::CheckFiles ) );

	if ( !unload && !std::filesystem::exists( library_path ) )
	{
		log_line( "neverlose_menu.dll is missing next to the loader.\n" );
		ui_note( 0.06f, static_cast<int>( InjectStage::MissingDll ), true, true );
		if ( hold ) hold_console( );
		return 1;
	}
	if ( !std::filesystem::exists( library_path ) && unload )
		log_line( "warning: neverlose_menu.dll is not next to the loader, "
			"continuing with the unload.\n" );

	DWORD process_id = explicit_pid;
	if ( explicit_pid && !process_alive( explicit_pid ) )
	{
		log_line( "pid %lu does not exist.\n", explicit_pid );
		ui_note( 0.1f, static_cast<int>( InjectStage::BadPid ), true, true );
		if ( hold ) hold_console( );
		return 1;
	}
	if ( !process_id )
		process_id = find_process( L"cs2.exe" );

	if ( !process_id && !unload && launch_game && !options.wait_for_game )
	{
		// Trusted Mode blocks third-party DLLs; launch through Steam with the
		// documented opt-out so the injection is accepted.
		log_line( "cs2.exe is not running -- starting it through Steam with\n"
			"-allow_third_party_software\n" );
		ui_note( 0.22f, static_cast<int>( InjectStage::LaunchGame ) );
		if ( !shell_execute_unelevated( L"steam://run/730//-allow_third_party_software" ) )
		{
			log_line( "  could not hand the launch to Steam. Start CS2 from Steam yourself\n"
				"  (Launch Options: -allow_third_party_software); the loader waits for it.\n" );
			ui_note( 0.22f, static_cast<int>( InjectStage::LaunchFailed ) );
		}
	}

	const auto wait_started = std::chrono::steady_clock::now();
	auto wait_progress = 0.18f;
	if ( !process_id )
	{
		log_line( "waiting for cs2.exe" );
		ui_note( wait_progress, static_cast<int>( InjectStage::Waiting ) );
	}
	while ( !( process_id = explicit_pid ? explicit_pid : find_process( L"cs2.exe" ) ) )
	{
		if ( ui_cancel() )
		{
			ui_note( wait_progress, static_cast<int>( InjectStage::Cancelled ), true, true );
			return 1;
		}
		if ( !wait_for_game && !launch_game )
		{
			log_line( "\ncs2.exe is not running. Start the game and rerun, "
				"or pass --wait.\n" );
			ui_note( wait_progress, static_cast<int>( InjectStage::NotRunning ), true, true );
			if ( hold ) hold_console( );
			return 1;
		}
		// The bar follows the real wait: it eases toward the inject step and only
		// finishes once cs2.exe is actually there.
		const float seconds = std::chrono::duration<float>(
			std::chrono::steady_clock::now() - wait_started ).count();
		wait_progress = 0.18f + 0.58f * ( seconds / ( seconds + 12.0f ) );
		ui_note( wait_progress, static_cast<int>( InjectStage::Waiting ) );
		log_line( "." );
		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
	}
	log_line( "\nfound %ls (pid %lu)\n",
		explicit_pid ? L"the target process" : L"cs2.exe", process_id );

	if ( unload )
	{
		if ( eject( process_id, L"neverlose_menu.dll" ) )
		{
			log_line( "unloaded.\n" );
			if ( hold ) hold_console( );
			return 0;
		}
		if ( hold ) hold_console( );
		return 1;
	}

	if ( find_module( process_id, L"neverlose_menu.dll" ) )
	{
		log_line( "already injected; use --unload to remove it.\n" );
		ui_note( 1.0f, static_cast<int>( InjectStage::AlreadyLoaded ), false, true );
		if ( hold ) hold_console( );
		return 0;
	}

	ui_note( 0.62f, static_cast<int>( InjectStage::Preparing ) );
	publish_present_rva();

	ui_note( 0.78f, static_cast<int>( InjectStage::Injecting ) );
	if ( !inject( process_id, library_path ) )
	{
		log_line( "\nIf the game was started without -allow_third_party_software,\n"
			"CS2 Trusted Mode refuses the module: close the game, then run this\n"
			"loader again (it starts CS2 through Steam with the flag itself).\n" );
		log_line( "the full run log: %%TEMP%%\\vesta\\loader.log\n" );
		ui_note( 0.78f, g_fail_stage, true, true );
		if ( hold ) hold_console( );
		return 1;
	}

	ui_note( 0.9f, static_cast<int>( InjectStage::Verifying ) );
	if ( !module_present( process_id, L"neverlose_menu.dll" ) )
	{
		log_line( "LoadLibraryW returned, but the module is not loaded -- Trusted Mode\n"
			"or the anti-cheat rejected it. Restart CS2 with\n"
			"-allow_third_party_software (Steam > CS2 > Properties > Launch Options).\n" );
		ui_note( 0.9f, static_cast<int>( InjectStage::ModuleRejected ), true, true );
		if ( hold ) hold_console( );
		return 1;
	}

	log_line( "injected. INSERT opens the menu, END hides everything.\n" );
	ui_note( 1.0f, static_cast<int>( InjectStage::Success ), false, true );
	if ( hold ) hold_console( );
	return 0;
}

int loader_console( const int argc, wchar_t** argv )
{
	InjectOptions options;
	for ( int index = 1; index < argc; ++index )
	{
		if ( ::wcscmp( argv[ index ], L"--unload" ) == 0 )
			options.unload = true;
		else if ( ::wcscmp( argv[ index ], L"--wait" ) == 0 )
			options.wait_for_game = true;
		else if ( ::wcscmp( argv[ index ], L"--no-launch" ) == 0 )
			options.launch_game = false;
		else if ( ::wcscmp( argv[ index ], L"--no-pause" ) == 0 )
			options.hold = false;
		else if ( ::wcscmp( argv[ index ], L"--pid" ) == 0 && index + 1 < argc )
			options.pid = static_cast<std::uint32_t>( ::wcstoul( argv[ ++index ], nullptr, 10 ) );
	}
	return run_inject( options, nullptr, nullptr, nullptr );
}

} // namespace nl
