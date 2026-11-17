#pragma once

#include <windows.h>

namespace nl_cheat {

// The Neverlose menu window handle; used to gate combat input while it is edited.
void set_menu_hwnd(HWND hwnd) noexcept;

// Starts the cheat engine on a background thread: attaches to cs2.exe, boots the
// feature workers and launches the in-game overlay. Retries while the game is not
// running; never blocks the menu.
void start();

// Injected build: the host process already is cs2.exe. No window and no overlay
// are created; features run in-process and the present hook draws the menu.
// `module` is the DLL instance (used to locate the shipped Lua scripts).
void start_internal(void* module);

// True while the engine is armed.
[[nodiscard]] bool running() noexcept;

// Stops the overlay and tears the engine down (the menu keeps running).
void shutdown() noexcept;

} // namespace nl_cheat
