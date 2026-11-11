// GUI entry: Win32 window + D3D11 swapchain + Dear ImGui loop, throttled while benchmarks run. The compare
// window is a second top-level window with its own ImGui context and swapchain on the same device.
#include "app/compare_ui.h"
#include "app/settings.h"
#include "app/settings_ui.h"
#include "app/health_ui.h"
#include "app/temps_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/elevation.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include "gpu/gpu_common.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wchar.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND, UINT, WPARAM, LPARAM, ImGuiIO&);

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

// Each window has its own ImGui context (and font atlas). g_fonts is swapped to the drawing window's fonts.
static ImGuiContext* s_main_ctx;
static ImGuiIO* s_main_io;
static ThemeFonts s_main_fonts;

// Tool windows (compare, settings, ...): static description + live state, indexed by ToolWindow.
struct ToolDesc {
    const wchar_t* title;
    int w, h;                   // initial size at 100% scaling
    void (*draw)();             // one frame of the contents
    void (*on_open)();          // may be null
    void (*on_close)();         // may be null
    bool accept_files;          // drag & drop of results files
};
static const ToolDesc s_tool_desc[TOOL_COUNT] = {
    { L"EzPcBench - Compare", 1360, 820, compare_ui_draw, compare_ui_on_open, nullptr, true },
    { L"EzPcBench - Settings", 640, 600, settings_ui_draw, nullptr, nullptr, false },
    { L"EzPcBench - Sensors", 1000, 820, temps_ui_draw, temps_ui_on_open, temps_ui_on_close, false },
    { L"EzPcBench - Disk health", 980, 720, health_ui_draw, health_ui_on_open, nullptr, false },
};

struct ToolWin {
    HWND hwnd;
    IDXGISwapChain* swapchain;
    ID3D11RenderTargetView* rtv;
    ImGuiContext* ctx;
    ImGuiIO* io;
    ThemeFonts fonts;
    UINT resize_w, resize_h;
    bool close, occluded, restyle;
};
static ToolWin s_tools[TOOL_COUNT];

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

// GPU test preview: the benchmark backend publishes small RGBA frames (gpu_preview_publish); the UI device
// copies the newest one into a texture. A CPU hand-off works across adapters (UI on the iGPU, test on the dGPU).
struct PreviewTex {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    u32 w, h, seq;
};
static PreviewTex s_prev;
static u8 s_prev_pixels[GPU_PREVIEW_MAX_W * GPU_PREVIEW_MAX_H * 4];

static void release_preview() {
    if (s_prev.srv) s_prev.srv->Release();
    if (s_prev.tex) s_prev.tex->Release();
    s_prev.srv = nullptr;
    s_prev.tex = nullptr;
    s_prev.w = s_prev.h = 0;
}

static void update_preview() {
    u32 w = 0, h = 0;
    if (!gpu_preview_acquire(&s_prev.seq, s_prev_pixels, &w, &h)) return;
    if (w == 0 || h == 0) { ui_set_preview(nullptr, 0, 0); return; }
    if (w != s_prev.w || h != s_prev.h) {
        release_preview();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(s_gfx.device->CreateTexture2D(&td, nullptr, &s_prev.tex)) ||
            FAILED(s_gfx.device->CreateShaderResourceView(s_prev.tex, nullptr, &s_prev.srv))) {
            release_preview();
            ui_set_preview(nullptr, 0, 0);
            return;
        }
        s_prev.w = w;
        s_prev.h = h;
    }
    s_gfx.ctx->UpdateSubresource(s_prev.tex, 0, nullptr, s_prev_pixels, w * 4, 0);
    ui_set_preview(s_prev.srv, w, h);
}

static void destroy_device() {
    release_preview();
    release_rtv();
    if (s_gfx.swapchain) s_gfx.swapchain->Release();
    if (s_gfx.ctx) s_gfx.ctx->Release();
    if (s_gfx.device) s_gfx.device->Release();
    s_gfx = {};
}

static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explicit io: messages can arrive while the compare context is current (e.g. its file dialog's modal loop).
    if (s_main_io && ImGui_ImplWin32_WndProcHandlerEx(hwnd, msg, wp, lp, *s_main_io)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) { s_gfx.resize_w = LOWORD(lp); s_gfx.resize_h = HIWORD(lp); }
            return 0;
        case WM_SETTINGCHANGE:      // Windows light/dark app mode toggled
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) theme_system_changed();
            break;
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
    f32 c[3];
    theme_bg_rgb(c);
    return RGB((int)(c[0] * 255 + 0.5f), (int)(c[1] * 255 + 0.5f), (int)(c[2] * 255 + 0.5f));
}

// Title bar matching the app background and mode (Windows 11; silently ignored elsewhere).
static void style_title_bar(HWND hwnd) {
    const BOOL dark = theme_is_dark();
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = bg_colorref(), text = dark ? RGB(0xE2, 0xE2, 0xE9) : RGB(0x19, 0x1C, 0x20);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

// App icon (resource 100, gui.rc) at the system's big (Alt+Tab, taskbar) or small (title bar) icon size.
static HICON app_icon(HINSTANCE inst, bool small_size) {     // not "small": a macro in the Windows headers
    const int w = GetSystemMetrics(small_size ? SM_CXSMICON : SM_CXICON), h = GetSystemMetrics(small_size ? SM_CYSMICON : SM_CYICON);
    return (HICON)LoadImageW(inst, MAKEINTRESOURCEW(100), IMAGE_ICON, w, h, LR_DEFAULTCOLOR);
}

// ---- tool windows ----------------------------------------------------------------------------------

static ToolWin* tool_of(HWND hwnd) {
    for (u32 i = 0; i < TOOL_COUNT; ++i)
        if (s_tools[i].hwnd == hwnd) return &s_tools[i];
    return nullptr;
}

static LRESULT WINAPI tool_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ToolWin* t = tool_of(hwnd);     // null for messages sent during CreateWindow
    if (t && t->io && ImGui_ImplWin32_WndProcHandlerEx(hwnd, msg, wp, lp, *t->io)) return true;
    switch (msg) {
        case WM_SIZE:
            if (t && wp != SIZE_MINIMIZED) { t->resize_w = LOWORD(lp); t->resize_h = HIWORD(lp); }
            return 0;
        case WM_DROPFILES: {        // only the compare window accepts files
            const HDROP drop = (HDROP)wp;
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH];
                if (DragQueryFileW(drop, i, path, MAX_PATH)) compare_ui_add_path(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) theme_system_changed();
            break;
        case WM_DPICHANGED: {
            const RECT* r = (const RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            if (t) t->close = true;     // torn down by the main loop, outside any ImGui frame
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static ID3D11RenderTargetView* back_buffer_rtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    sc->GetBuffer(0, IID_PPV_ARGS(&back));
    s_gfx.device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    return rtv;
}

static bool create_tool_swapchain(ToolWin* t) {
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    bool ok = SUCCEEDED(s_gfx.device->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) &&
              SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)));
    if (ok) {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = t->hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ok = SUCCEEDED(factory->CreateSwapChain(s_gfx.device, &sd, &t->swapchain));
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgi_dev) dxgi_dev->Release();
    if (ok) t->rtv = back_buffer_rtv(t->swapchain);
    return ok;
}

static void tool_close(u32 id) {
    ToolWin* t = &s_tools[id];
    if (!t->hwnd) return;
    if (t->ctx) {
        if (s_tool_desc[id].on_close) s_tool_desc[id].on_close();
        ImGui::SetCurrentContext(t->ctx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(t->ctx);
        ImGui::SetCurrentContext(s_main_ctx);
    }
    if (t->rtv) t->rtv->Release();
    if (t->swapchain) t->swapchain->Release();
    const HWND hwnd = t->hwnd;
    *t = {};
    DestroyWindow(hwnd);
}

// Opens tool window `id`, or brings it to the front if it is already open.
static void tool_open(HINSTANCE inst, u32 id) {
    ToolWin* t = &s_tools[id];
    const ToolDesc* d = &s_tool_desc[id];
    if (t->hwnd) {
        if (IsIconic(t->hwnd)) ShowWindow(t->hwnd, SW_RESTORE);
        SetForegroundWindow(t->hwnd);
        return;
    }
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, tool_wnd_proc, 0, 0, inst, app_icon(inst, false), LoadCursorW(nullptr, IDC_ARROW),
                           CreateSolidBrush(bg_colorref()), nullptr, L"ezpcbench_tool", app_icon(inst, true) };
        registered = RegisterClassExW(&wc) != 0;
    }
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = (int)(d->w * s_dpi_scale), h = (int)(d->h * s_dpi_scale);
    if (w > (work.right - work.left) * 9 / 10) w = (work.right - work.left) * 9 / 10;
    if (h > (work.bottom - work.top) * 9 / 10) h = (work.bottom - work.top) * 9 / 10;
    t->hwnd = CreateWindowW(L"ezpcbench_tool", d->title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                            nullptr, nullptr, inst, nullptr);
    if (!t->hwnd || !create_tool_swapchain(t)) { tool_close(id); return; }
    style_title_bar(t->hwnd);
    if (d->accept_files) {
        // Accept files dropped from a non-elevated Explorer (UIPI blocks these messages by default).
        DragAcceptFiles(t->hwnd, TRUE);
        ChangeWindowMessageFilterEx(t->hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(t->hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(t->hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
    }

    t->ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(t->ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ui_apply_style(s_dpi_scale);
    ImGui_ImplWin32_Init(t->hwnd);
    ImGui_ImplDX11_Init(s_gfx.device, s_gfx.ctx);
    theme_load_fonts();
    t->fonts = g_fonts;
    t->io = &io;
    g_fonts = s_main_fonts;
    ImGui::SetCurrentContext(s_main_ctx);

    if (d->on_open) d->on_open();
    ShowWindow(t->hwnd, SW_SHOW);
}

static bool tool_visible(u32 id) {
    const ToolWin* t = &s_tools[id];
    return t->hwnd && !(t->occluded && t->swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED);
}

// One frame of tool window `id`. `vsync`: present with sync interval 1 (when nothing else did this loop).
static void tool_frame(u32 id, bool vsync) {
    ToolWin* t = &s_tools[id];
    if (t->resize_w && t->resize_h) {
        if (t->rtv) { t->rtv->Release(); t->rtv = nullptr; }
        t->swapchain->ResizeBuffers(0, t->resize_w, t->resize_h, DXGI_FORMAT_UNKNOWN, 0);
        t->resize_w = t->resize_h = 0;
        t->rtv = back_buffer_rtv(t->swapchain);
    }
    ImGui::SetCurrentContext(t->ctx);
    g_fonts = t->fonts;
    if (t->restyle) {
        ui_apply_style(s_dpi_scale);
        style_title_bar(t->hwnd);
        t->restyle = false;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    s_tool_desc[id].draw();
    ImGui::Render();
    f32 clear[4] = { 0, 0, 0, 1 };
    theme_bg_rgb(clear);
    s_gfx.ctx->OMSetRenderTargets(1, &t->rtv, nullptr);
    s_gfx.ctx->ClearRenderTargetView(t->rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    t->occluded = t->swapchain->Present(vsync ? 1 : 0, 0) == DXGI_STATUS_OCCLUDED;
    g_fonts = s_main_fonts;
    ImGui::SetCurrentContext(s_main_ctx);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int) {
    ImGui_ImplWin32_EnableDpiAwareness();

    // Always ask for admin first; continue with warnings if the user declines.
    const bool no_elevate = cmdline && wcsstr(cmdline, L"--no-elevate");
    if (!no_elevate && !is_elevated() && relaunch_elevated(L"")) return 0;

    // Theme first: the window class brush and title bar use its colors. Saved mode (default: follow Windows),
    // --dark / --light override it for this session.
    const ThemeMode saved_theme = settings_load_theme();
    theme_set_mode(cmdline && wcsstr(cmdline, L"--dark") ? THEME_DARK : cmdline && wcsstr(cmdline, L"--light") ? THEME_LIGHT
                                                                                                                : saved_theme);
    theme_take_change();

    sysinfo_init();
    runner_init();
    ui_init();

    s_dpi_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, app_icon(inst, false), LoadCursorW(nullptr, IDC_ARROW),
                       CreateSolidBrush(bg_colorref()), nullptr, L"ezpcbench", app_icon(inst, true) };
    RegisterClassExW(&wc);
    wchar_t title[96];
    swprintf(title, 96, L"EzPcBench%s", is_elevated() ? L" (Administrator)" : L"");
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int win_w = (int)(1280 * s_dpi_scale), win_h = (int)(820 * s_dpi_scale);
    if (win_w > (work.right - work.left) * 9 / 10) win_w = (work.right - work.left) * 9 / 10;
    if (win_h > (work.bottom - work.top) * 9 / 10) win_h = (work.bottom - work.top) * 9 / 10;
    HWND hwnd = CreateWindowW(wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
                              nullptr, nullptr, inst, nullptr);
    style_title_bar(hwnd);
    if (!create_device(hwnd)) {
        MessageBoxW(nullptr, L"Failed to create a Direct3D 11 device.", L"EzPcBench", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    s_main_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ui_apply_style(s_dpi_scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(s_gfx.device, s_gfx.ctx);
    theme_load_fonts();
    s_main_fonts = g_fonts;
    s_main_io = &io;
    if (cmdline) {
        if (const wchar_t* d = wcsstr(cmdline, L"--duration ")) ui_set_duration((u32)_wtoi(d + 11));
        if (wcsstr(cmdline, L"--dev")) ui_set_dev(true);          // show the smoke-test suite
        if (const wchar_t* o = wcsstr(cmdline, L"--only ")) {      // dev: select only these test ids
            char ids[512] = {};
            u32 n = 0;
            for (o += 7; *o && *o != L' ' && n + 1 < sizeof(ids); ++o) ids[n++] = (char)*o;
            if (strstr(ids, "smoke")) ui_set_dev(true);             // smoke tests are only visible in dev mode
            ui_select_only(ids);
        }
        if (wcsstr(cmdline, L"--autorun")) ui_start_run();   // run the default selection immediately
        // --compare [files...]: open the compare window with these result files.
        int argc = 0;
        if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            static const wchar_t* const tool_names[TOOL_COUNT] = { L"compare", L"settings", L"temps", L"health" };
            for (int i = 1; i < argc; ++i) {
                if (!wcscmp(argv[i], L"--tool") && i + 1 < argc) {     // dev: --tool settings opens that window
                    for (u32 t = 0; t < TOOL_COUNT; ++t)
                        if (!wcscmp(argv[i + 1], tool_names[t]) || (t == TOOL_TEMPS && !wcscmp(argv[i + 1], L"sensors")))
                            tool_open(inst, t);
                    continue;
                }
                if (wcscmp(argv[i], L"--compare") != 0) continue;
                for (int k = i + 1; k < argc && wcsncmp(argv[k], L"--", 2) != 0; ++k) compare_ui_add_path(argv[k]);
                tool_open(inst, TOOL_COMPARE);
            }
            LocalFree(argv);
        }
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
        for (u32 i = 0; i < TOOL_COUNT; ++i)
            if (s_tools[i].close) tool_close(i);

        const f64 now = timer_now();
        const bool running = runner_is_running();
        const f64 interval = running ? 1.0 / 15.0 : (now - last_input < 1.0 ? 0.0 : 0.25);
        const bool occluded = s_gfx.occluded && s_gfx.swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
        bool tool_vis[TOOL_COUNT], any_tool = false;
        for (u32 i = 0; i < TOOL_COUNT; ++i) any_tool |= tool_vis[i] = tool_visible(i);
        if (now - last_frame < interval || (occluded && !any_tool)) {
            const f64 wait = occluded && !any_tool ? 0.1 : interval - (now - last_frame);
            MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(wait * 1000.0) + 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        last_frame = now;
        if (s_dpi_changed || theme_take_change()) {
            ui_apply_style(s_dpi_scale);
            style_title_bar(hwnd);
            s_dpi_changed = false;
            for (u32 i = 0; i < TOOL_COUNT; ++i) s_tools[i].restyle = true;
        }
        if (occluded) {     // main window minimized / hidden: only the tool windows draw (the first one vsyncs)
            bool vsync = true;
            for (u32 i = 0; i < TOOL_COUNT; ++i)
                if (tool_vis[i]) { tool_frame(i, vsync); vsync = false; }
            continue;
        }
        s_gfx.occluded = false;

        if (s_gfx.resize_w && s_gfx.resize_h) {
            release_rtv();
            s_gfx.swapchain->ResizeBuffers(0, s_gfx.resize_w, s_gfx.resize_h, DXGI_FORMAT_UNKNOWN, 0);
            s_gfx.resize_w = s_gfx.resize_h = 0;
            create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        update_preview();
        ui_draw();
        ImGui::Render();

        f32 clear[4] = { 0, 0, 0, 1 };
        theme_bg_rgb(clear);
        s_gfx.ctx->OMSetRenderTargets(1, &s_gfx.rtv, nullptr);
        s_gfx.ctx->ClearRenderTargetView(s_gfx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        s_gfx.occluded = s_gfx.swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;

        const u32 requests = ui_take_tool_requests();
        for (u32 i = 0; i < TOOL_COUNT; ++i)
            if (requests & (1u << i)) tool_open(inst, i);
        for (u32 i = 0; i < TOOL_COUNT; ++i)
            if (tool_vis[i]) tool_frame(i, false);
    }

    if (runner_is_running()) runner_cancel();
    runner_wait();
    ui_shutdown();
    for (u32 i = 0; i < TOOL_COUNT; ++i) tool_close(i);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
