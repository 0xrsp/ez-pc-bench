// Sensor overlay (see overlay.h). Twice a second: GDI draws white anti-aliased text on black into a DIB, the
// gray level becomes text coverage over a translucent dark panel (premultiplied alpha), UpdateLayeredWindow
// puts it on screen. WS_EX_TRANSPARENT + WS_EX_LAYERED make it click-through.
#include "app/overlay.h"
#include "core/bench.h"
#include "core/runner.h"
#include "core/sensors.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

constexpr u32 OVL_LINES = 24;
constexpr DWORD OVL_REFRESH_MS = 500;

// Shared with the UI thread (s_cfg_lock).
static SRWLOCK s_cfg_lock = SRWLOCK_INIT;
static char s_ids[2048];
static OverlayCorner s_corner = OVL_TOP_RIGHT;

static HANDLE s_thread, s_quit, s_wake;
static std::atomic<bool> s_visible;

static const char* const DEFAULT_IDS = "cpu.load,cpu.max_mhz,emi.cpu_pkg,temp.*,gpu0.load,bat.rate_w";

// ---- drawing (overlay thread) -------------------------------------------------------------------------

struct Line { wchar_t label[32], value[32]; };

static void widen(const char* s, wchar_t* out, int size) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, size); }

static void format_value(const SensorChannel* c, wchar_t* out, u32 size) {
    if (!c->valid) { swprintf(out, size, L"\x2014"); return; }
    wchar_t unit[8];
    widen(c->unit, unit, 8);
    if (!strcmp(c->unit, "MHz")) swprintf(out, size, L"%.2f GHz", c->value / 1000.0f);
    else if (!strcmp(c->unit, "%")) swprintf(out, size, L"%.0f %%", c->value);
    else if (!strcmp(c->unit, "W")) swprintf(out, size, L"%.1f W", c->value);
    else if (c->unit[0] == '\xC2') swprintf(out, size, L"%.0f %s", c->value, unit);      // degrees
    else swprintf(out, size, L"%.*f %s", c->decimals, c->value, unit);
}

// Lines for the configured ids (patterns allowed: "temp.*" adds every temperature), FPS first during GPU tests.
static u32 collect(Line* lines) {
    u32 n = 0;
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    const char* unit = g_live.unit.load(std::memory_order_relaxed);
    if (runner_is_running() && cur >= 0 && unit && !strcmp(unit, "fps") &&
        (g_benches[cur].suite == SUITE_GPU_VK || g_benches[cur].suite == SUITE_GPU_D3D12)) {
        swprintf(lines[n].label, 32, L"FPS");
        swprintf(lines[n].value, 32, L"%.0f", g_live.current.load(std::memory_order_relaxed));
        ++n;
    }
    static SensorChannel ch[CH_MAX];
    const u32 count = sensors_channels(ch, CH_MAX);
    char ids[2048];
    AcquireSRWLockShared(&s_cfg_lock);
    snprintf(ids, sizeof(ids), "%s", s_ids[0] ? s_ids : DEFAULT_IDS);
    ReleaseSRWLockShared(&s_cfg_lock);
    // Same order as the channel list (the Sensors tab).
    for (u32 i = 0; i < count && n < OVL_LINES; ++i) {
        if (!sensors_channel_matches(&ch[i], ids)) continue;
        widen(ch[i].label, lines[n].label, 32);
        format_value(&ch[i], lines[n].value, 32);
        ++n;
    }
    if (!n) {
        swprintf(lines[0].label, 32, L"Sensors");
        swprintf(lines[0].value, 32, L"\x2026");
        n = 1;
    }
    return n;
}

struct Gdi {
    HDC dc;
    HFONT font;
    i32 font_px;
    HBITMAP bmp;
    u32* bits;
    i32 w, h;
};
static Gdi s_gdi;

static void ensure_font(i32 px) {
    if (s_gdi.font && s_gdi.font_px == px) return;
    if (s_gdi.font) DeleteObject(s_gdi.font);
    s_gdi.font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    s_gdi.font_px = px;
}

static bool ensure_bitmap(i32 w, i32 h) {
    if (s_gdi.bmp && s_gdi.w == w && s_gdi.h == h) return true;
    if (s_gdi.bmp) DeleteObject(s_gdi.bmp);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;     // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    s_gdi.bmp = CreateDIBSection(s_gdi.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    s_gdi.bits = (u32*)bits;
    s_gdi.w = w;
    s_gdi.h = h;
    return s_gdi.bmp != nullptr;
}

static void render(HWND hwnd) {
    Line lines[OVL_LINES];
    const u32 n = collect(lines);
    const f32 scale = GetDpiForWindow(hwnd) / 96.0f;
    ensure_font((i32)(14.0f * scale + 0.5f));
    if (!s_gdi.dc) s_gdi.dc = CreateCompatibleDC(nullptr);
    SelectObject(s_gdi.dc, s_gdi.font);

    // Layout: labels left, values right-aligned in a second column.
    TEXTMETRICW tm;
    GetTextMetricsW(s_gdi.dc, &tm);
    const i32 pad = (i32)(10.0f * scale), gap = (i32)(16.0f * scale), line_h = tm.tmHeight + (i32)(2.0f * scale);
    i32 label_w = 0, value_w = (i32)(60.0f * scale);
    for (u32 i = 0; i < n; ++i) {
        SIZE sz;
        GetTextExtentPoint32W(s_gdi.dc, lines[i].label, (int)wcslen(lines[i].label), &sz);
        label_w = sz.cx > label_w ? sz.cx : label_w;
        GetTextExtentPoint32W(s_gdi.dc, lines[i].value, (int)wcslen(lines[i].value), &sz);
        value_w = sz.cx > value_w ? sz.cx : value_w;
    }
    const i32 w = pad * 2 + label_w + gap + value_w, h = pad * 2 + line_h * (i32)n - (i32)(2.0f * scale);
    if (!ensure_bitmap(w, h)) return;
    SelectObject(s_gdi.dc, s_gdi.bmp);
    memset(s_gdi.bits, 0, (size_t)w * h * 4);
    SetBkMode(s_gdi.dc, TRANSPARENT);
    SetTextColor(s_gdi.dc, RGB(255, 255, 255));
    for (u32 i = 0; i < n; ++i) {
        const i32 y = pad + line_h * (i32)i;
        TextOutW(s_gdi.dc, pad, y, lines[i].label, (int)wcslen(lines[i].label));
        RECT r = { pad + label_w + gap, y, w - pad, y + line_h };
        DrawTextW(s_gdi.dc, lines[i].value, -1, &r, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    GdiFlush();

    // Coverage -> premultiplied BGRA over a translucent panel with rounded corners.
    const u32 bg_a = 190, bg[3] = { 26, 22, 20 };                      // B, G, R
    const u32 label_c[3] = { 200, 196, 190 }, value_c[3] = { 255, 255, 255 };
    const i32 split = pad + label_w + gap / 2;
    const f32 rad = 8.0f * scale;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            u32* p = &s_gdi.bits[y * w + x];
            const u32 b = *p & 0xFF, g = (*p >> 8) & 0xFF, r = (*p >> 16) & 0xFF;
            const u32 cov = r > g ? (r > b ? r : b) : (g > b ? g : b);
            // Corner rounding: distance from the corner circle's center.
            const f32 cx = x < rad ? rad - x - 0.5f : x >= w - rad ? x + 0.5f - (w - rad) : 0.0f;
            const f32 cy = y < rad ? rad - y - 0.5f : y >= h - rad ? y + 0.5f - (h - rad) : 0.0f;
            if (cx > 0.0f && cy > 0.0f && cx * cx + cy * cy > rad * rad) { *p = 0; continue; }
            const u32* tc = x < split ? label_c : value_c;
            const u32 a = bg_a + (255 - bg_a) * cov / 255;
            u32 out = a << 24;
            for (u32 k = 0; k < 3; ++k) {
                const u32 c = (bg[k] * (255 - cov) + tc[k] * cov) / 255;
                out |= (c * a / 255) << (8 * k);
            }
            *p = out;
        }
    }

    // Corner of the primary monitor's work area.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    AcquireSRWLockShared(&s_cfg_lock);
    const OverlayCorner corner = s_corner;
    ReleaseSRWLockShared(&s_cfg_lock);
    const i32 margin = (i32)(12.0f * scale);
    const RECT wa = mi.rcWork;
    POINT pos;
    pos.x = corner == OVL_TOP_LEFT || corner == OVL_BOTTOM_LEFT ? wa.left + margin : wa.right - margin - w;
    pos.y = corner == OVL_TOP_LEFT || corner == OVL_TOP_RIGHT ? wa.top + margin : wa.bottom - margin - h;
    SIZE size = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, nullptr, &pos, &size, s_gdi.dc, &src, 0, &blend, ULW_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // stay above new top-most windows
}

static LRESULT WINAPI overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI overlay_thread(void*) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ezpcbench_overlay";
    RegisterClassExW(&wc);      // fails harmlessly when already registered by an earlier overlay thread
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"EzPcBench overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    sensors_start();
    render(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const HANDLE events[2] = { s_quit, s_wake };
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjects(2, events, FALSE, OVL_REFRESH_MS, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        render(hwnd);
    }
    DestroyWindow(hwnd);
    sensors_stop();
    if (s_gdi.bmp) DeleteObject(s_gdi.bmp);
    if (s_gdi.font) DeleteObject(s_gdi.font);
    if (s_gdi.dc) DeleteDC(s_gdi.dc);
    s_gdi = {};
    return 0;
}

// ---- control (UI thread) ------------------------------------------------------------------------------

void overlay_set(bool visible, const char* channel_ids, OverlayCorner corner) {
    AcquireSRWLockExclusive(&s_cfg_lock);
    snprintf(s_ids, sizeof(s_ids), "%s", channel_ids ? channel_ids : "");
    s_corner = corner < OVL_CORNER_COUNT ? corner : OVL_TOP_RIGHT;
    ReleaseSRWLockExclusive(&s_cfg_lock);
    if (visible && !s_thread) {
        s_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s_thread = CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr);
    } else if (!visible && s_thread) {
        overlay_shutdown();
    } else if (s_thread) {
        SetEvent(s_wake);       // redraw now with the new settings
    }
    s_visible.store(s_thread != nullptr);
}

bool overlay_visible() { return s_visible.load(); }

void overlay_shutdown() {
    if (!s_thread) return;
    SetEvent(s_quit);
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    CloseHandle(s_quit);
    CloseHandle(s_wake);
    s_thread = s_quit = s_wake = nullptr;
    s_visible.store(false);
}
