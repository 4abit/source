#include <render/menu/menu.hpp>

namespace
{
	std::atomic<HWND> g_nl_window{ nullptr };
	std::atomic<bool> g_menu_open{ false };
}

bool menu_t::initialize(HWND /*hwnd*/)
{
	return true;
}

void menu_t::poll_hotkey()
{
}

void menu_t::draw()
{
}

void menu_t::map_pointer_to_layout(float&, float&, float, float) const noexcept
{
}

void menu_t::map_pointer_to_screen(float&, float&, float, float) const noexcept
{
}

bool menu_t::is_open() const noexcept
{
	if ( g_menu_open.load( std::memory_order_acquire ) )
		return true;

	const HWND window = g_nl_window.load( std::memory_order_acquire );
	if ( !window || !::IsWindow( window ) )
		return false;
	if ( !::IsWindowVisible( window ) )
		return false;
	return ::GetForegroundWindow() == window;
}

void menu_t::set_nl_window( HWND hwnd ) noexcept
{
	g_nl_window.store( hwnd, std::memory_order_release );
}

void menu_t::set_open( bool open ) noexcept
{
	g_menu_open.store( open, std::memory_order_release );
}
