#pragma once

#include <windows.h>

#include <atomic>

// Shim over the vesta menu: the real menu is the Neverlose ImGui window.
// All interaction points of the vesta pipeline call into this class; only
// is_open() carries meaning (the NL window being edited blocks combat input).
class menu_t
{
  public:
    bool initialize(HWND hwnd);
    void poll_hotkey();
    void draw();
    void map_pointer_to_layout(float &x, float &y, float display_width, float display_height) const noexcept;
    void map_pointer_to_screen(float &x, float &y, float display_width, float display_height) const noexcept;
    void close() noexcept
    {
        this->m_open.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool is_open() const noexcept;

    // Registered by the NL bridge so is_open() can track the real menu window.
    static void set_nl_window(HWND hwnd) noexcept;

    // In the in-game build the menu has no window; the hook publishes its
    // visible state directly.
    static void set_open(bool open) noexcept;

  private:
    std::atomic<bool> m_open{};
};
