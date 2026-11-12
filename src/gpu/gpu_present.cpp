// On-screen mode helpers shared by both GPU backends: the benchmark window and the display log.
#include "gpu/gpu_present.h"
#include "core/results.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>

// ---- window -------------------------------------------------------------------------------------

static LRESULT WINAPI bench_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PresentWindow* w = (PresentWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CLOSE:
        if (w) w->closed = true;
        return 0;                                   // destroyed by present_window_close
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && w) w->closed = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (w && w->fullscreen && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone would open the (absent) menu
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool present_window_open(PresentWindow* w, const BenchParams* p, const char* api, const char* test_name,
                         char* err, u32 err_size) {
    *w = {};
    static const wchar_t* const CLASS = L"ezpcbench_bench";
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc), CS_OWNDC, bench_wnd_proc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(100)),
                       LoadCursorW(nullptr, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), nullptr, CLASS, nullptr };
    RegisterClassExW(&wc);      // fails harmlessly when already registered

    w->prev_foreground = GetForegroundWindow();
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(w->prev_foreground, MONITOR_DEFAULTTOPRIMARY), &mi);
    w->fullscreen = p->gpu_fullscreen;
    DWORD style, ex_style = 0;
    RECT r;
    if (w->fullscreen) {
        style = WS_POPUP;
        ex_style = WS_EX_TOPMOST;
        r = mi.rcMonitor;
    } else {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        const RECT wa = mi.rcWork;
        RECT c = { 0, 0, (LONG)p->gpu_width, (LONG)p->gpu_height };
        AdjustWindowRectEx(&c, style, FALSE, ex_style);
        LONG cw = c.right - c.left, ch = c.bottom - c.top;
        if (cw > wa.right - wa.left) cw = wa.right - wa.left;
        if (ch > wa.bottom - wa.top) ch = wa.bottom - wa.top;
        r.left = wa.left + (wa.right - wa.left - cw) / 2;
        r.top = wa.top + (wa.bottom - wa.top - ch) / 2;
        r.right = r.left + cw;
        r.bottom = r.top + ch;
    }
    wchar_t title[160];
    _snwprintf_s(title, ARRAY_COUNT(title), _TRUNCATE, L"EzPcBench - %hs (%hs) - Esc stops the test", test_name, api);
    w->hwnd = CreateWindowExW(ex_style, CLASS, title, style, r.left, r.top, r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, inst, nullptr);
    if (!w->hwnd) {
        snprintf(err, err_size, "creating the benchmark window failed (%lu)", GetLastError());
        return false;
    }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    ShowWindow(w->hwnd, SW_SHOW);
    SetForegroundWindow(w->hwnd);
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    w->w = (u32)(cr.right - cr.left);
    w->h = (u32)(cr.bottom - cr.top);
    present_window_pump(w);
    return w->w > 0 && w->h > 0;
}

bool present_window_pump(PresentWindow* w) {
    MSG msg;
    while (PeekMessageW(&msg, w->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !w->closed;
}

void present_window_close(PresentWindow* w) {
    if (!w->hwnd) return;
    const bool was_foreground = GetForegroundWindow() == w->hwnd;
    DestroyWindow(w->hwnd);
    w->hwnd = nullptr;
    MSG msg;    // drain what DestroyWindow posted to this thread
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    if (was_foreground && w->prev_foreground && IsWindow(w->prev_foreground)) SetForegroundWindow(w->prev_foreground);
}

const char* present_mode_name(const BenchParams* p) {
    return !p->gpu_onscreen ? "offscreen" : p->gpu_fullscreen ? "fullscreen" : "windowed";
}

const char* gpu_test_name(GpuTestId test) {
    static const char* const names[GT_COUNT] = {
#define X(id, e, g, gid, name, flags) name,
        GPU_TESTS(X)
#undef X
    };
    return test < GT_COUNT ? names[test] : "";
}

// ---- display log ---------------------------------------------------------------------------------

constexpr u32 LOG_MAX = 1u << 20;
static f64 s_present_t[LOG_MAX];    // NaN = not logged
static f64 s_display_t[LOG_MAX];
static f32 s_sorted[LOG_MAX];
static u64 s_first_id;
static u32 s_count;                 // highest logged index + 1

void present_log_begin(u64 first_id) {
    s_first_id = first_id;
    s_count = 0;
}

static i64 log_index(u64 id) {
    if (id < s_first_id || id - s_first_id >= LOG_MAX) return -1;
    const u32 i = (u32)(id - s_first_id);
    while (s_count <= i) {
        s_present_t[s_count] = NAN;
        s_display_t[s_count] = NAN;
        ++s_count;
    }
    return i;
}

void present_log_presented(u64 id, f64 t) {
    const i64 i = log_index(id);
    if (i >= 0) s_present_t[i] = t;
}

void present_log_displayed(u64 id, f64 t) {
    const i64 i = log_index(id);
    if (i >= 0) s_display_t[i] = t;
}

static f64 sorted_stat(f32* v, u32 n, f64* avg) {     // returns p99, *avg = mean
    f64 sum = 0.0;
    for (u32 i = 0; i < n; ++i) sum += v[i];
    *avg = n ? sum / n : 0.0;
    std::sort(v, v + n);
    const u32 k = (u32)(0.99 * (n - 1) + 0.5);
    return n ? v[k < n ? k : n - 1] : 0.0;
}

void present_log_report(BenchResult* out) {
    u32 presented = 0, displayed = 0, lat_n = 0;
    for (u32 i = 0; i < s_count; ++i) {
        presented += !isnan(s_present_t[i]);
        if (isnan(s_display_t[i])) continue;
        ++displayed;
        if (!isnan(s_present_t[i])) s_sorted[lat_n++] = (f32)((s_display_t[i] - s_present_t[i]) * 1000.0);
    }
    if (!presented || !displayed) return;
    res_add(out, "Displayed frames", "%", 100.0 * displayed / presented);
    f64 avg;
    if (lat_n) {
        const f64 p99 = sorted_stat(s_sorted, lat_n, &avg);
        res_add(out, "Display latency avg", "ms", avg);
        res_add(out, "Display latency p99", "ms", p99);
    }
    u32 n = 0;
    f64 last = NAN;
    for (u32 i = 0; i < s_count; ++i) {
        const f64 t = s_display_t[i];
        if (isnan(t)) continue;
        if (!isnan(last) && t > last) s_sorted[n++] = (f32)((t - last) * 1000.0);
        if (isnan(last) || t > last) last = t;
    }
    if (n) {
        const f64 p99 = sorted_stat(s_sorted, n, &avg);
        res_add(out, "Displayed frame time avg", "ms", avg);
        res_add(out, "Displayed frame time p99", "ms", p99);
    }
}
