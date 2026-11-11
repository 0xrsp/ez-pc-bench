// GUI entry: Win32 window + D3D11 swapchain + Dear ImGui loop, throttled while benchmarks run.
#include "app/theme.h"
#include "app/ui.h"
#include "core/elevation.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wchar.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

struct Gfx {
    ID3D11Device* device;
    ID3D11DeviceContext* ctx;
    IDXGISwapChain* swapchain;
    ID3D11RenderTargetView* rtv;
    UINT resize_w, resize_h;
    bool occluded;
};
static Gfx s_gfx;
static f32 s_dpi_scale = 1.0f;
static bool s_dpi_changed = false;

static void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    s_gfx.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    s_gfx.device->CreateRenderTargetView(back, nullptr, &s_gfx.rtv);
    back->Release();
}

static void release_rtv() {
    if (s_gfx.rtv) { s_gfx.rtv->Release(); s_gfx.rtv = nullptr; }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                               &sd, &s_gfx.swapchain, &s_gfx.device, nullptr, &s_gfx.ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &sd,
                                           &s_gfx.swapchain, &s_gfx.device, nullptr, &s_gfx.ctx);
    if (FAILED(hr)) return false;
    create_rtv();
    return true;
}

static void destroy_device() {
    release_rtv();
    if (s_gfx.swapchain) s_gfx.swapchain->Release();
    if (s_gfx.ctx) s_gfx.ctx->Release();
    if (s_gfx.device) s_gfx.device->Release();
    s_gfx = {};
}

static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { s_gfx.resize_w = LOWORD(lp); s_gfx.resize_h = HIWORD(lp); }
            return 0;
        case WM_DPICHANGED: {
            s_dpi_scale = HIWORD(wp) / 96.0f;
            s_dpi_changed = true;
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static COLORREF bg_colorref() {
    return RGB((int)(TH_BG_RGB[0] * 255 + 0.5f), (int)(TH_BG_RGB[1] * 255 + 0.5f), (int)(TH_BG_RGB[2] * 255 + 0.5f));
}

// Dark title bar matching the app background (Windows 11; silently ignored elsewhere).
static void style_title_bar(HWND hwnd) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = bg_colorref(), text = RGB(0xEE, 0xEA, 0xFB);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    // Always ask for admin first; continue with warnings if the user declines.
    const bool no_elevate = cmdline && wcsstr(cmdline, L"--no-elevate");
    if (!no_elevate && !is_elevated() && relaunch_elevated(L"")) return 0;

    sysinfo_init();
    runner_init();
    ui_init();

    s_dpi_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, nullptr, LoadCursorW(nullptr, IDC_ARROW),
                       CreateSolidBrush(bg_colorref()), nullptr, L"ezpcbench", nullptr };
    RegisterClassExW(&wc);
    wchar_t title[96];
    swprintf(title, 96, L"ez-pc-bench%s", is_elevated() ? L" (Administrator)" : L"");
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int win_w = (int)(1280 * s_dpi_scale), win_h = (int)(820 * s_dpi_scale);
    if (win_w > (work.right - work.left) * 9 / 10) win_w = (work.right - work.left) * 9 / 10;
    if (win_h > (work.bottom - work.top) * 9 / 10) win_h = (work.bottom - work.top) * 9 / 10;
    HWND hwnd = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
                              nullptr, nullptr, inst, nullptr);
    style_title_bar(hwnd);
    if (!create_device(hwnd)) {
        MessageBoxW(nullptr, L"Failed to create a Direct3D 11 device.", L"ez-pc-bench", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ui_apply_style(s_dpi_scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(s_gfx.device, s_gfx.ctx);
    theme_load_fonts();
    if (cmdline) {
        if (const wchar_t* d = wcsstr(cmdline, L"--duration ")) ui_set_duration((u32)_wtoi(d + 11));
        if (wcsstr(cmdline, L"--autorun")) ui_start_run();   // run the default selection immediately
    }

    // Frame pacing: ~15 FPS while a run is active (stay out of the benchmarks' way),
    // vsync while the user interacts, 4 FPS when idle.
    f64 last_frame = 0.0, last_input = timer_now();
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
            last_input = timer_now();
        }
        if (done || ui_wants_quit()) break;

        const f64 now = timer_now();
        const bool running = runner_is_running();
        const f64 interval = running ? 1.0 / 15.0 : (now - last_input < 1.0 ? 0.0 : 0.25);
        const bool occluded = s_gfx.occluded && s_gfx.swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
        if (now - last_frame < interval || occluded) {
            const f64 wait = occluded ? 0.1 : interval - (now - last_frame);
            MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(wait * 1000.0) + 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        s_gfx.occluded = false;
        last_frame = now;

        if (s_gfx.resize_w && s_gfx.resize_h) {
            release_rtv();
            s_gfx.swapchain->ResizeBuffers(0, s_gfx.resize_w, s_gfx.resize_h, DXGI_FORMAT_UNKNOWN, 0);
            s_gfx.resize_w = s_gfx.resize_h = 0;
            create_rtv();
        }
        if (s_dpi_changed) {
            ui_apply_style(s_dpi_scale);
            s_dpi_changed = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ui_draw();
        ImGui::Render();

        const f32 clear[4] = { TH_BG_RGB[0], TH_BG_RGB[1], TH_BG_RGB[2], 1.0f };
        s_gfx.ctx->OMSetRenderTargets(1, &s_gfx.rtv, nullptr);
        s_gfx.ctx->ClearRenderTargetView(s_gfx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        s_gfx.occluded = s_gfx.swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    if (runner_is_running()) runner_cancel();
    runner_wait();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
