// Diagnostic probe: proves whether a plain DLL can be loaded into cs2.exe at all.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <iterator>

namespace {

	void log_line( const char* text )
	{
		wchar_t temporary[ MAX_PATH ]{};
		if ( !::GetTempPathW( static_cast<DWORD>( std::size( temporary ) ), temporary ) )
			return;
		char narrow[ MAX_PATH ]{};
		::WideCharToMultiByte( CP_ACP, 0, temporary, -1, narrow,
			static_cast<int>( std::size( narrow ) ), nullptr, nullptr );
		char path[ MAX_PATH ]{};
		::wsprintfA( path, "%svesta\\probe.log", narrow );
		FILE* file = std::fopen( path, "a" );
		if ( !file )
			return;
		std::fprintf( file, "pid=%lu tick=%llu %s\n", ::GetCurrentProcessId(),
			static_cast<unsigned long long>( ::GetTickCount64() ), text );
		std::fclose( file );
	}

	DWORD WINAPI worker( LPVOID )
	{
		log_line( "probe_thread_started" );
		return 0;
	}

} // namespace

BOOL WINAPI DllMain( HINSTANCE instance, const DWORD reason, LPVOID )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		::DisableThreadLibraryCalls( instance );
		log_line( "probe_dllmain" );
		::CreateThread( nullptr, 0, worker, nullptr, 0, nullptr );
	}
	return TRUE;
}
