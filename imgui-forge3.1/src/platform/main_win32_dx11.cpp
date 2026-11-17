// Windows viewer: Win32 + DirectX 11 (the same stack an in-game overlay would use).
//
// With --screenshot it plays scripted input instead of the real mouse and saves one frame:
//   neverlose_menu.exe --scale 1.5 --tab 2 --click 900,40,5 --frames 40 --screenshot out.bmp
// --picker SLOT opens the inventory's skin picker on that loadout slot (14 = AWP);
// --picker SLOT:ITEM also selects that catalogue item, and --side 0|1 picks the team.
// Events (x,y in client pixels, f = frame): --move x,y,f  --click x,y,f  --rclick x,y,f
// --down x,y,f / --up x,y,f (left button held in between)  --wheel d,f  --text f,abc
// --mdown b,f / --mup b,f (mouse button b: 3 = M4, 4 = M5)  --toggle f (Insert)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "menu/backdrop.h"
#include "menu/blur.h"
#include "menu/esp_model.h"
#include "menu/fonts.h"
#include "menu/menu.h"
#include "menu/model_preview.h"
#include "menu/style.h"
#include "platform/blur_dx11.h"

// Cheat backend: the vesta pipeline attached to cs2.exe, driven by this menu's config.
#include "port/cheat_bridge.hpp"
#include "port/config_bridge.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

constexpr DWORD k_window_style = WS_OVERLAPPEDWINDOW;
// Client area at scale 1: the menu with the backdrop around it.
constexpr float k_client_w = 872.0f;
constexpr float k_client_h = 624.0f;
// Menu size relative to the reference screenshot on a 100% (96 DPI) display.
constexpr float k_default_scale = 1.5f;
// Smallest gap kept around the menu when the default size is reduced to fit the screen, at 96 DPI.
// Leaves room for the hint line under the menu.
constexpr float k_fit_margin = 40.0f;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
int g_back_w = 0, g_back_h = 0;
bool g_occluded = false;
UINT g_resize_w = 0;
UINT g_resize_h = 0;
float g_dpi_scale = 1.0f;
nl::Menu* g_menu = nullptr;
bool g_scripted = false;

enum class Ev { move, click, rclick, down, up, wheel, text, mdown, mup, toggle };
struct ScriptEvent {
    Ev type;
    int frame;
    float x = 0.0f, y = 0.0f;
    std::string text;
};

struct Script {
    const char* screenshot = nullptr;
    float scale = 0.0f;
    int width = 0, height = 0;
    int frames = 30;
    int tab = -1, subtab = 0;
    int picker = -1;
    int item = -1;
    int side = -1;
    bool expand = false;
    std::vector<ScriptEvent> events;
};

Script parse_script()
{
    Script s;
    for (int i = 1; i + 1 < __argc; ++i) {
        const char* a = __argv[i];
        const char* v = __argv[i + 1];
        ScriptEvent e{};
        bool event = true;
        if (!std::strcmp(a, "--screenshot")) { s.screenshot = v; event = false; }
        else if (!std::strcmp(a, "--scale")) { s.scale = std::strtof(v, nullptr); event = false; }
        else if (!std::strcmp(a, "--size")) { std::sscanf(v, "%dx%d", &s.width, &s.height); event = false; }
        else if (!std::strcmp(a, "--frames")) { s.frames = std::atoi(v); event = false; }
        else if (!std::strcmp(a, "--tab")) { s.tab = std::atoi(v); event = false; }
        else if (!std::strcmp(a, "--subtab")) { s.subtab = std::atoi(v); event = false; }
        else if (!std::strcmp(a, "--picker")) {
            // "slot" or "slot:item", the item being a catalogue index, so one exact skin
            // can be put on screen without scripting a click on the right grid card.
            const char* colon = std::strchr(v, ':');
            s.picker = std::atoi(v);
            if (colon)
                s.item = std::atoi(colon + 1);
            event = false;
        }
        else if (!std::strcmp(a, "--side")) { s.side = std::atoi(v); event = false; }
        else if (!std::strcmp(a, "--expand")) { s.expand = std::atoi(v) != 0; event = false; }
        else if (!std::strcmp(a, "--move")) e.type = Ev::move;
        else if (!std::strcmp(a, "--click")) e.type = Ev::click;
        else if (!std::strcmp(a, "--rclick")) e.type = Ev::rclick;
        else if (!std::strcmp(a, "--down")) e.type = Ev::down;
        else if (!std::strcmp(a, "--up")) e.type = Ev::up;
        else if (!std::strcmp(a, "--wheel")) e.type = Ev::wheel;
        else if (!std::strcmp(a, "--text")) e.type = Ev::text;
        else if (!std::strcmp(a, "--mdown")) e.type = Ev::mdown;
        else if (!std::strcmp(a, "--mup")) e.type = Ev::mup;
        else if (!std::strcmp(a, "--toggle")) e.type = Ev::toggle;
        else continue;
        ++i;
        if (!event)
            continue;
        switch (e.type) {
        case Ev::wheel: std::sscanf(v, "%f,%d", &e.y, &e.frame); break;
        case Ev::mdown:
        case Ev::mup: std::sscanf(v, "%f,%d", &e.x, &e.frame); break;
        case Ev::toggle: e.frame = std::atoi(v); break;
        case Ev::text: {
            const char* comma = std::strchr(v, ',');
            e.frame = std::atoi(v);
            e.text = comma ? comma + 1 : "";
            break;
        }
        default: std::sscanf(v, "%f,%f,%d", &e.x, &e.y, &e.frame); break;
        }
        s.events.push_back(e);
    }
    return s;
}

void feed_script(const Script& s, int frame, nl::Menu& menu)
{
    ImGuiIO& io = ImGui::GetIO();
    for (const ScriptEvent& e : s.events) {
        switch (e.type) {
        case Ev::move:
            if (e.frame == frame) io.AddMousePosEvent(e.x, e.y);
            break;
        case Ev::click:
        case Ev::rclick: {
            const int b = e.type == Ev::click ? 0 : 1;
            if (e.frame - 1 == frame) io.AddMousePosEvent(e.x, e.y);
            if (e.frame == frame) io.AddMouseButtonEvent(b, true);
            if (e.frame + 1 == frame) io.AddMouseButtonEvent(b, false);
            break;
        }
        case Ev::down:
            if (e.frame - 1 == frame) io.AddMousePosEvent(e.x, e.y);
            if (e.frame == frame) io.AddMouseButtonEvent(0, true);
            break;
        case Ev::up:
            if (e.frame == frame) { io.AddMousePosEvent(e.x, e.y); io.AddMouseButtonEvent(0, false); }
            break;
        case Ev::wheel:
            if (e.frame == frame) io.AddMouseWheelEvent(0.0f, e.y);
            break;
        case Ev::text:
            if (e.frame == frame) io.AddInputCharactersUTF8(e.text.c_str());
            break;
        case Ev::mdown:
        case Ev::mup:
            if (e.frame == frame) io.AddMouseButtonEvent(static_cast<int>(e.x), e.type == Ev::mdown);
            break;
        case Ev::toggle:
            if (e.frame == frame) menu.toggle_visible();
            break;
        }
    }
}

bool write_bmp(const char* path)
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging));
    if (ok) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        ok = SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            const int w = static_cast<int>(desc.Width), h = static_cast<int>(desc.Height);
            const int stride = (w * 3 + 3) & ~3;
            FILE* f = std::fopen(path, "wb");
            ok = f != nullptr;
            if (f) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + static_cast<DWORD>(stride * h);
                ih.biSize = sizeof(ih);
                ih.biWidth = w;
                ih.biHeight = h;
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                std::fwrite(&fh, sizeof(fh), 1, f);
                std::fwrite(&ih, sizeof(ih), 1, f);
                std::vector<unsigned char> row(static_cast<size_t>(stride), 0);
                for (int y = h - 1; y >= 0; --y) {
                    const auto* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                    for (int x = 0; x < w; ++x) {
                        row[static_cast<size_t>(x * 3 + 0)] = src[x * 4 + 2];
                        row[static_cast<size_t>(x * 3 + 1)] = src[x * 4 + 1];
                        row[static_cast<size_t>(x * 3 + 2)] = src[x * 4 + 0];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
            }
            g_context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

void create_render_target()
{
    ID3D11Texture2D* back_buffer = nullptr;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_rtv);
        D3D11_TEXTURE2D_DESC desc{};
        back_buffer->GetDesc(&desc);
        g_back_w = static_cast<int>(desc.Width);
        g_back_h = static_cast<int>(desc.Height);
        back_buffer->Release();
    }
}

void cleanup_render_target()
{
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool create_device(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_context);
    if (FAILED(hr)) // No usable GPU (e.g. remote desktop or VM): fall back to the software rasterizer.
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap_chain, &g_device, &level, &g_context);
    if (FAILED(hr))
        return false;

    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(g_swap_chain->GetParent(IID_PPV_ARGS(&factory)))) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    create_render_target();
    return true;
}

void cleanup_device()
{
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (!g_scripted && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g_resize_w = LOWORD(lparam);
            g_resize_h = HIWORD(lparam);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        const float s = nl::Menu::min_scale;
        RECT rc{ 0, 0, static_cast<LONG>(nl::style::layout::width * s) + 40,
                 static_cast<LONG>(nl::style::layout::height * s) + 40 };
        AdjustWindowRectEx(&rc, k_window_style, FALSE, 0);
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = rc.right - rc.left;
        info->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_DPICHANGED: {
        const float dpi_scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
        if (g_menu)
            g_menu->rescale(dpi_scale / g_dpi_scale);
        g_dpi_scale = dpi_scale;
        const auto* r = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_INSERT && (lparam & (1 << 30)) == 0 && g_menu && !g_scripted)
            g_menu->toggle_visible();
        break;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

namespace {
// The desktop harness had no crash reporting: a fault simply killed the process and the
// only evidence was an exit code. This writes the faulting module with its offset and a
// raw stack to %TEMP%\vesta\harness_crash.log, which llvm-addr2line resolves afterwards.
LONG WINAPI harness_crash_filter(EXCEPTION_POINTERS* info)
{
    static bool reported = false;
    if (reported || !info || !info->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    reported = true;

    wchar_t directory[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, directory))
        return EXCEPTION_CONTINUE_SEARCH;
    ::lstrcatW(directory, L"vesta");
    ::CreateDirectoryW(directory, nullptr);
    ::lstrcatW(directory, L"\\harness_crash.log");
    const HANDLE file = ::CreateFileW(directory, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto* record = info->ExceptionRecord;
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    wchar_t module[MAX_PATH]{};
    std::uintptr_t base = 0;
    HMODULE owner = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &owner) && owner) {
        base = reinterpret_cast<std::uintptr_t>(owner);
        ::GetModuleFileNameW(owner, module, MAX_PATH);
    }
    char narrow[MAX_PATH]{};
    for (int i = 0; i < MAX_PATH - 1 && module[i]; ++i)
        narrow[i] = module[i] < 128 ? static_cast<char>(module[i]) : '?';

    void* frames[24]{};
    const USHORT count = ::CaptureStackBackTrace(0, 24, frames, nullptr);

    char line[2048]{};
    int written = std::snprintf(line, sizeof(line), "code=0x%08lX %s+0x%llX stack:",
                                static_cast<unsigned long>(record->ExceptionCode), narrow,
                                static_cast<unsigned long long>(address - base));
    for (USHORT i = 0; i < count && written > 0 && written < static_cast<int>(sizeof(line)) - 24; ++i)
        written += std::snprintf(line + written, sizeof(line) - static_cast<size_t>(written),
                                 " %llX", static_cast<unsigned long long>(
                                              reinterpret_cast<std::uintptr_t>(frames[i])));
    if (written > 0 && written < static_cast<int>(sizeof(line)) - 2) {
        line[written++] = '\n';
        line[written] = 0;
    }
    DWORD ignored = 0;
    ::WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &ignored, nullptr);
    ::CloseHandle(file);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    ::AddVectoredExceptionHandler(1, harness_crash_filter);

    // Render in physical pixels and scale the menu by the monitor DPI instead, so Windows doesn't
    // stretch a low-resolution frame on scaled displays.
    ImGui_ImplWin32_EnableDpiAwareness();
    g_dpi_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"NeverloseImGuiMenu";
    RegisterClassExW(&wc);

    // Default scale for this display, reduced to a 5% step that still fits the work area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT frame{};
    AdjustWindowRectEx(&frame, k_window_style, FALSE, 0);
    const float avail_w = static_cast<float>((work.right - work.left) - (frame.right - frame.left));
    const float avail_h = static_cast<float>((work.bottom - work.top) - (frame.bottom - frame.top));
    const float margin = 2.0f * k_fit_margin * g_dpi_scale;
    const float fit = std::min((avail_w - margin) / nl::style::layout::width, (avail_h - margin) / nl::style::layout::height);
    const Script script = parse_script();
    g_scripted = script.screenshot != nullptr;
    float scale = std::clamp(std::floor(std::min(k_default_scale * g_dpi_scale, fit) * 20.0f) / 20.0f,
                             nl::Menu::min_scale, nl::Menu::max_scale);
    if (g_scripted)
        scale = std::clamp(script.scale > 0.0f ? script.scale : 1.0f, nl::Menu::min_scale, nl::Menu::max_scale);

    RECT rc{ 0, 0, static_cast<LONG>(std::min(std::round(k_client_w * scale), avail_w)),
             static_cast<LONG>(std::min(std::round(k_client_h * scale), avail_h)) };
    if (g_scripted) {
        rc.right = script.width > 0 ? script.width : static_cast<LONG>(std::round(k_client_w * scale));
        rc.bottom = script.height > 0 ? script.height : static_cast<LONG>(std::round(k_client_h * scale));
    }
    AdjustWindowRectEx(&rc, k_window_style, FALSE, 0);
    const int window_w = rc.right - rc.left;
    const int window_h = rc.bottom - rc.top;
    const int x = work.left + std::max(0, static_cast<int>(work.right - work.left - window_w) / 2);
    const int y = work.top + std::max(0, static_cast<int>(work.bottom - work.top - window_h) / 2);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Neverlose \u2014 ImGui", k_window_style, x, y, window_w,
                                window_h, nullptr, nullptr, instance, nullptr);

    // Dark title bar (Windows 10 20H1+) and caption color matching the menu (Windows 11).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(17, 18, 24);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(hwnd, L"Failed to create a Direct3D 11 device.", L"Neverlose \u2014 ImGui", MB_ICONERROR);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    if (!g_scripted) {
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    nl::apply_imgui_style();
    nl::load_fonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    // The ESP model preview renders offscreen on this device; it is borrowed and
    // released again below, before the device itself goes away.
    nl::esp_model::set_device(g_device, g_context);
    // Same device for the Inventory's model inspector, which owns its own camera and so
    // its own preview instance.
    nl::model_preview::set_device(g_device, g_context);
    const bool blur = nl::blur_dx11::init(g_device, g_context);

    RECT client{};
    GetClientRect(hwnd, &client);
    nl::Menu menu;
    g_menu = &menu;
    menu.init(ImVec2(std::floor((static_cast<float>(client.right) - std::round(nl::style::layout::width * scale)) * 0.5f),
                     std::floor((static_cast<float>(client.bottom) - std::round(nl::style::layout::height * scale)) * 0.5f)),
              scale);
    if (script.tab >= 0)
        menu.set_tab(script.tab, script.subtab);
    // Opens the skin picker straight away, which is also what makes a screenshot of the
    // 3D model inspector reproducible without scripting a click on the right card.
    if (script.side >= 0)
        menu.config().inventory.side = script.side == 1 ? 1 : 0;
    if (script.picker >= 0)
        menu.open_picker(script.picker, script.item);
    if (script.expand)
        menu.set_inspect_fullscreen(true);

    if (!g_scripted) {
        nl_cheat::set_menu_hwnd(hwnd);
        nl_cheat::start();
    }

    bool done = false;
    for (int frame = 0; !done; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (!g_scripted && g_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resize_w != 0 && g_resize_h != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_render_target();
        }

        if (blur)
            nl::blur::set_backend(nl::blur_dx11::prepare(g_back_w, g_back_h));
        ImGui_ImplDX11_NewFrame();
        if (g_scripted) {
            io.DisplaySize = ImVec2(static_cast<float>(g_back_w), static_cast<float>(g_back_h));
            io.DeltaTime = 1.0f / 60.0f;
            feed_script(script, frame, menu);
        } else {
            ImGui_ImplWin32_NewFrame();
        }
        ImGui::NewFrame();
        nl::draw_backdrop(ImGui::GetBackgroundDrawList(), io.DisplaySize, menu.visible());
        menu.render();
        if (!g_scripted)
            nl_cheat::sync_config(menu.config());
        if (g_scripted) // cursor marker in screenshots
            ImGui::GetForegroundDrawList()->AddCircle(io.MousePos, 4.0f, io.MouseDown[0] ? IM_COL32(255, 60, 60, 255) : IM_COL32(255, 220, 0, 255), 12, 1.5f);
        ImGui::Render();

        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (g_scripted) {
            if (frame + 1 >= script.frames) {
                write_bmp(script.screenshot);
                break;
            }
            continue;
        }
        const HRESULT hr = g_swap_chain->Present(1, 0);
        g_occluded = hr == DXGI_STATUS_OCCLUDED;
    }

    nl::blur::set_backend({});
    nl::blur_dx11::shutdown();
    // Drop the previews before the device they borrow and before ImGui forgets the
    // textures they handed out.
    nl::model_preview::release();
    nl::esp_model::release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    menu.shutdown();
    g_menu = nullptr;
    ImGui::DestroyContext();

    if (!g_scripted)
        nl_cheat::shutdown();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
