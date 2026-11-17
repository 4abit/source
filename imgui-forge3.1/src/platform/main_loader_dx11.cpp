// Borderless loader window. Launch runs the inject job; --unload, --wait,
// --no-launch, --pid, --no-pause and --console keep the old console path.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "loader/loader.h"
#include "menu/fonts.h"
#include "menu/menu.h"
#include "menu/style.h"
#include "platform/inject.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

constexpr DWORD k_window_style = WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU;
constexpr float k_default_scale = 1.25f;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0;
UINT g_resize_h = 0;
bool g_occluded = false;
float g_dpi_scale = 1.0f;
nl::Loader* g_loader = nullptr;

struct JobState {
    std::mutex mu;
    float progress = 0.0f;
    int stage = 0;
    bool failed = false;
    bool done = false;
    bool active = false;
};

JobState g_job;
std::atomic<bool> g_cancel{ false };
std::thread g_worker;

const char* stage_text(int stage)
{
    switch (static_cast<nl::InjectStage>(stage)) {
    case nl::InjectStage::CheckFiles: return "Проверка файлов...";
    case nl::InjectStage::MissingDll: return "Нет neverlose_menu.dll рядом с лоадером";
    case nl::InjectStage::LaunchGame: return "Запуск Counter-Strike 2...";
    case nl::InjectStage::LaunchFailed: return "Не удалось запустить игру через Steam";
    case nl::InjectStage::Waiting: return "Ожидание cs2.exe...";
    case nl::InjectStage::Cancelled: return "Отменено";
    case nl::InjectStage::Preparing: return "Подготовка...";
    case nl::InjectStage::Injecting: return "Инжект...";
    case nl::InjectStage::Verifying: return "Проверка модуля...";
    case nl::InjectStage::AlreadyLoaded: return "Готово";
    case nl::InjectStage::Success: return "Готово";
    case nl::InjectStage::OpenFailed: return "Не удалось открыть процесс";
    case nl::InjectStage::InjectFailed: return "Модуль не загрузился";
    case nl::InjectStage::ModuleRejected: return "Модуль отклонён. Перезапустите CS2";
    case nl::InjectStage::BadPid: return "Процесс не найден";
    case nl::InjectStage::NotRunning: return "cs2.exe не запущен";
    }
    return "Ошибка";
}

void on_report(void* user, float progress, int stage, bool failed, bool done)
{
    auto* job = static_cast<JobState*>(user);
    std::lock_guard<std::mutex> lock(job->mu);
    job->progress = progress;
    job->stage = stage;
    job->failed = failed;
    job->done = done;
    job->active = !done;
}

struct JobView {
    float progress = 0.0f;
    int stage = 0;
    bool failed = false;
    bool done = false;
    bool active = false;
};

JobView job_view()
{
    std::lock_guard<std::mutex> lock(g_job.mu);
    return { g_job.progress, g_job.stage, g_job.failed, g_job.done, g_job.active };
}

void clear_job_if_done()
{
    std::lock_guard<std::mutex> lock(g_job.mu);
    if (!g_job.done)
        return;
    g_job.done = false;
    g_job.failed = false;
    g_job.active = false;
}

void start_job()
{
    if (g_worker.joinable())
        g_worker.join();
    {
        std::lock_guard<std::mutex> lock(g_job.mu);
        g_job.progress = 0.06f;
        g_job.stage = static_cast<int>(nl::InjectStage::CheckFiles);
        g_job.failed = false;
        g_job.done = false;
        g_job.active = true;
    }
    g_cancel.store(false);
    g_worker = std::thread([] {
        nl::InjectOptions options;
        options.hold = false;
        options.launch_game = true;
        nl::run_inject(options, on_report, &g_job, &g_cancel);
    });
}

void stop_job()
{
    g_cancel.store(true);
    if (g_worker.joinable())
        g_worker.join();
}

bool want_console(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--unload") || !std::wcscmp(argv[i], L"--wait") ||
            !std::wcscmp(argv[i], L"--no-launch") || !std::wcscmp(argv[i], L"--no-pause") ||
            !std::wcscmp(argv[i], L"--pid") || !std::wcscmp(argv[i], L"--console"))
            return true;
    }
    return false;
}

void attach_console()
{
    if (!::AttachConsole(ATTACH_PARENT_PROCESS) && !::AllocConsole())
        return;
    FILE* stream = nullptr;
#if defined(_MSC_VER)
    ::freopen_s(&stream, "CONOUT$", "w", stdout);
    ::freopen_s(&stream, "CONOUT$", "w", stderr);
    ::freopen_s(&stream, "CONIN$", "r", stdin);
#else
    stream = std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
    std::freopen("CONIN$", "r", stdin);
    (void)stream;
#endif
}

void create_render_target()
{
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
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
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_context);
    if (FAILED(hr))
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
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
        return 1;

    switch (msg) {
    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ::ScreenToClient(hwnd, &p);
        if (g_loader && g_loader->caption_hit(ImVec2(static_cast<float>(p.x), static_cast<float>(p.y))))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            g_resize_w = LOWORD(lparam);
            g_resize_h = HIWORD(lparam);
        }
        return 0;
    case WM_DPICHANGED: {
        const float dpi_scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
        nl::style::ui_scale *= dpi_scale / g_dpi_scale;
        nl::apply_imgui_style();
        g_dpi_scale = dpi_scale;
        const auto* r = reinterpret_cast<const RECT*>(lparam);
        ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv && want_console(argc, argv)) {
        attach_console();
        const int code = nl::loader_console(argc, argv);
        ::LocalFree(argv);
        return code;
    }
    ::LocalFree(argv);

    ImGui_ImplWin32_EnableDpiAwareness();
    g_dpi_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC | CS_DROPSHADOW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = L"NeverloseForgeLoader";
    ::RegisterClassExW(&wc);

    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const float avail_w = static_cast<float>(work.right - work.left);
    const float avail_h = static_cast<float>(work.bottom - work.top);
    const float fit = std::min(avail_w * 0.9f / nl::Loader::width, avail_h * 0.9f / nl::Loader::height);
    float scale = std::clamp(std::floor(std::min(k_default_scale * g_dpi_scale, fit) * 20.0f) / 20.0f, 0.75f, 3.0f);
    nl::style::ui_scale = scale;

    const int window_w = static_cast<int>(std::round(nl::Loader::width * scale));
    const int window_h = static_cast<int>(std::round(nl::Loader::height * scale));
    const int x = work.left + std::max(0, static_cast<int>(work.right - work.left - window_w) / 2);
    const int y = work.top + std::max(0, static_cast<int>(work.bottom - work.top - window_h) / 2);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Neverlose Loader", k_window_style, x, y, window_w, window_h,
                                  nullptr, nullptr, instance, nullptr);

    const int corners = 2; // DWMWCP_ROUND
    ::DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));

    if (!create_device(hwnd)) {
        cleanup_device();
        ::MessageBoxW(hwnd, L"Failed to create a Direct3D 11 device.", L"Neverlose Loader", MB_ICONERROR);
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    nl::apply_imgui_style();
    nl::load_fonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    nl::Loader loader;
    g_loader = &loader;
    loader.init();
    loader.bind_launch(true);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resize_w != 0 && g_resize_h != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_render_target();
        }

        const JobView job = job_view();
        if (loader.busy() && (job.active || job.done))
            loader.set_job(job.progress, stage_text(job.stage), job.done, job.failed);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        loader.render();
        ImGui::Render();

        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (job.done && !loader.busy())
            clear_job_if_done();
        if (loader.take_launch())
            start_job();
        if (loader.take_minimize())
            ::ShowWindow(hwnd, SW_MINIMIZE);
        const nl::Loader::Dismiss dismiss = loader.dismiss();
        if (dismiss.amount > 0.0f) {
            RECT client{};
            ::GetClientRect(hwnd, &client);
            const int w = client.right;
            const int h = client.bottom;
            const int rw = std::max(1, static_cast<int>(std::lround(w * dismiss.scale)));
            const int rh = std::max(1, static_cast<int>(std::lround(h * dismiss.scale)));
            const int rx = (w - rw) / 2;
            const int ry = (h - rh) / 2 + static_cast<int>(std::lround(dismiss.slide));
            const int rad = std::max(2, static_cast<int>(std::lround(18.0f * nl::style::ui_scale * dismiss.scale)));
            // The region is the shrinking card. Pixels outside it are not the window,
            // so the fade does not leave a black rectangle behind.
            ::SetWindowRgn(hwnd, ::CreateRoundRectRgn(rx, ry, rx + rw + 1, ry + rh + 1, rad, rad), TRUE);
        }
        if (loader.close_requested())
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);

        const HRESULT hr = g_swap_chain->Present(1, 0);
        g_occluded = hr == DXGI_STATUS_OCCLUDED;
    }

    stop_job();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    loader.shutdown();
    g_loader = nullptr;
    ImGui::DestroyContext();

    cleanup_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
