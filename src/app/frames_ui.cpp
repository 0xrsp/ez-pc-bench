// Frame capture window: target picker, live progress, result numbers, frame-time graph, save / compare.
#include "app/frames_ui.h"
#include "app/compare_ui.h"
#include "app/frames_graph.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/timer.h"
#include "frames/frame_capture.h"
#include "imgui.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

struct AppEntry {
    u32 pid;
    char exe[64];
    char title[96];
};

struct FramesUi {
    AppEntry apps[128];
    u32 app_count;
    i32 app;                    // -1 = "busiest presenter"
    char typed[128];            // name or pid typed by hand (wins over the list)
    int seconds = 20;
    bool allowed;
    char status[400];
    wchar_t saved[MAX_PATH];    // file of the current result once saved
    u32 live_presents;          // live rate
    f64 live_t;
    f32 live_rate;
};
static FramesUi s_fu;

// ---- target list --------------------------------------------------------------------------------------

static BOOL CALLBACK add_window(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    wchar_t title[96];
    if (GetWindowTextW(hwnd, title, ARRAY_COUNT(title)) == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    for (u32 i = 0; i < s_fu.app_count; ++i)
        if (s_fu.apps[i].pid == pid) return TRUE;
    if (s_fu.app_count >= ARRAY_COUNT(s_fu.apps)) return FALSE;
    AppEntry* e = &s_fu.apps[s_fu.app_count];
    e->pid = pid;
    e->exe[0] = 0;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &len)) {
            const wchar_t* name = wcsrchr(path, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1, e->exe, sizeof(e->exe), nullptr, nullptr);
        }
        CloseHandle(p);
    }
    if (!e->exe[0]) return TRUE;            // a process we can't even query (protected): skip
    WideCharToMultiByte(CP_UTF8, 0, title, -1, e->title, sizeof(e->title), nullptr, nullptr);
    s_fu.app_count++;
    return TRUE;
}

static void refresh_apps() {
    const u32 old_pid = s_fu.app >= 0 && (u32)s_fu.app < s_fu.app_count ? s_fu.apps[s_fu.app].pid : 0;
    s_fu.app_count = 0;
    EnumWindows(add_window, 0);
    s_fu.app = -1;
    for (u32 i = 0; i < s_fu.app_count; ++i)
        if (s_fu.apps[i].pid == old_pid) s_fu.app = (i32)i;
}

void frames_ui_on_open() {
    s_fu.allowed = capture_allowed();
    refresh_apps();
}

void frames_ui_on_close() {
    if (g_capture.state.load() == CAP_RUNNING) {
        capture_stop();
        capture_wait();
    }
}

// ---- actions --------------------------------------------------------------------------------------------

static void start() {
    char target[128];
    if (s_fu.typed[0]) snprintf(target, sizeof(target), "%s", s_fu.typed);
    else if (s_fu.app >= 0) snprintf(target, sizeof(target), "%u", s_fu.apps[s_fu.app].pid);
    else snprintf(target, sizeof(target), "*");
    char err[320];
    s_fu.saved[0] = 0;
    s_fu.status[0] = 0;
    s_fu.live_presents = 0;
    s_fu.live_t = timer_now();
    s_fu.live_rate = 0.0f;
    if (!capture_start(target, (f64)s_fu.seconds, err, sizeof(err))) snprintf(s_fu.status, sizeof(s_fu.status), "%s", err);
}

static bool save() {
    if (s_fu.saved[0]) return true;
    const FrameCapture* c = capture_result();
    char name[128];
    capture_default_name(c, name, sizeof(name));
    wchar_t dir[MAX_PATH], wname[128], path[MAX_PATH];
    ui_results_dir(dir);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, ARRAY_COUNT(wname));
    swprintf(path, MAX_PATH, L"%s\\%s.toon.txt", dir, wname);
    for (u32 k = 2; GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && k < 1000; ++k)
        swprintf(path, MAX_PATH, L"%s\\%s_%u.toon.txt", dir, wname, k);
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    const bool ok = capture_write_toon(path, c);
    snprintf(s_fu.status, sizeof(s_fu.status), ok ? "Saved %s" : "Failed to write %s", path8);
    if (ok) wcscpy_s(s_fu.saved, path);
    return ok;
}

// ---- drawing --------------------------------------------------------------------------------------------

static void section(const char* title) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

static void big_value(const char* label, f64 value, const char* unit, const char* help) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.6f));
    ImGui::Text("%.1f", value);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", unit);
    ImGui::EndGroup();
    theme_help(help);
}

static void draw_target(bool running) {
    section("Target");
    const f32 fs = ImGui::GetFontSize();
    ImGui::BeginDisabled(running);
    char preview[192];
    if (s_fu.app >= 0 && (u32)s_fu.app < s_fu.app_count)
        snprintf(preview, sizeof(preview), "%s (%u) - %s", s_fu.apps[s_fu.app].exe, s_fu.apps[s_fu.app].pid, s_fu.apps[s_fu.app].title);
    else
        snprintf(preview, sizeof(preview), "Busiest presenting process");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Application");
    ImGui::SameLine(fs * 8.0f);
    ImGui::SetNextItemWidth(fs * 34.0f);
    if (ImGui::BeginCombo("##app", preview)) {
        if (ImGui::Selectable("Busiest presenting process", s_fu.app < 0)) s_fu.app = -1;
        for (u32 i = 0; i < s_fu.app_count; ++i) {
            char item[192];
            snprintf(item, sizeof(item), "%s (%u) - %s##%u", s_fu.apps[i].exe, s_fu.apps[i].pid, s_fu.apps[i].title, i);
            if (ImGui::Selectable(item, s_fu.app == (i32)i)) s_fu.app = (i32)i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (theme_button("Refresh")) refresh_apps();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Or by name");
    ImGui::SameLine(fs * 8.0f);
    ImGui::SetNextItemWidth(fs * 16.0f);
    ImGui::InputTextWithHint("##typed", "game.exe or a pid", s_fu.typed, sizeof(s_fu.typed));
    theme_help("Exe name (extension optional) or process id. Overrides the list. Several processes with the same "
               "name are all recorded and the one that presents most is reported.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Duration");
    ImGui::SameLine(fs * 8.0f);
    ImGui::SetNextItemWidth(fs * 16.0f);
    ImGui::SliderInt("##seconds", &s_fu.seconds, 5, 300, "%d s");
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (!running) {
        if (theme_button_primary("Start capture")) start();
    } else {
        if (theme_button("Stop")) capture_stop();
        ImGui::SameLine();
        const f64 now = timer_now(), elapsed = now - g_capture.start;
        if (now - s_fu.live_t >= 0.5) {
            const u32 p = g_capture.presents.load();
            s_fu.live_rate = (f32)((p - s_fu.live_presents) / (now - s_fu.live_t));
            s_fu.live_presents = p;
            s_fu.live_t = now;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Recording %.0f / %.0f s, %u presents (~%.0f per second; ETW delivers about once a second)", elapsed,
                    g_capture.seconds, g_capture.presents.load(), s_fu.live_rate);
        theme_progress((f32)(elapsed / g_capture.seconds), fs * 0.3f);
    }
}

static void draw_result() {
    const FrameCapture* c = capture_result();
    section("Result");
    ImGui::Text("%s (pid %u)", c->process, c->pid);
    ImGui::SameLine();
    ImGui::TextDisabled("  %s%s, %.1f s, %u frames", c->api, c->tearing < 0 ? "" : c->tearing ? ", tearing allowed" : ", no tearing",
                        c->seconds, c->frames);
    if (c->sync_interval >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled(", sync interval %d%s", c->sync_interval, c->sync_interval ? " (vsync)" : " (vsync off)");
    }
    if (c->dropped) ImGui::TextColored(g_pal.warn, "%u events were lost: frame times around them are too long.", c->dropped);

    ImGui::Spacing();
    const f32 gap = ImGui::GetFontSize() * 2.5f;
    big_value("Average", c->avg_fps, "fps", "Frames divided by the capture time.");
    ImGui::SameLine(0.0f, gap);
    big_value("1% low", c->p99_ms > 0.0 ? 1000.0 / c->p99_ms : 0.0, "fps", "1000 / the 99th percentile frame time: 99 % of frames were faster.");
    ImGui::SameLine(0.0f, gap);
    big_value("0.1% low", c->p999_ms > 0.0 ? 1000.0 / c->p999_ms : 0.0, "fps", "1000 / the 99.9th percentile frame time.");
    ImGui::SameLine(0.0f, gap);
    big_value("1% low average", c->low1_avg_fps, "fps", "Average fps of the slowest 1 % of frames (CapFrameX's \"1% low average\").");
    ImGui::SameLine(0.0f, gap);
    big_value("0.1% low average", c->low01_avg_fps, "fps", "Average fps of the slowest 0.1 % of frames.");

    ImGui::Spacing();
    theme_push_numbers();
    ImGui::Text("Frame time  avg %.2f   median %.2f   p95 %.2f   p99 %.2f   p99.9 %.2f   max %.2f ms", c->avg_ms, c->p50_ms,
                c->p95_ms, c->p99_ms, c->p999_ms, c->max_ms);
    if (c->present_call_ms == c->present_call_ms) ImGui::Text("Present() call avg %.3f ms", c->present_call_ms);
    ImGui::PopFont();

    ImGui::Spacing();
    const FrameSeries s = { c->process, c->frame_ms, c->frames };
    f32 y_max = (f32)c->p999_ms * 1.25f;
    if (y_max < (f32)c->p50_ms * 2.0f) y_max = (f32)c->p50_ms * 2.0f;
    frame_graph("##frames", &s, 1, y_max, ImGui::GetFontSize() * 12.0f);

    ImGui::Spacing();
    if (theme_button_primary("Save")) save();
    theme_help("Writes the capture as a results file (TOON) into the results folder, with every frame time.");
    ImGui::SameLine();
    if (theme_button("Open in compare") && save()) {
        compare_ui_add_path(s_fu.saved);
        ui_request_tool(TOOL_COMPARE);
    }
    theme_help("Saves the capture and adds it to the compare window, next to other captures or runs.");
}

void frames_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##frames", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::TextDisabled("Records when another application presents frames (Windows ETW present events, like PresentMon) "
                        "and reports fps, frame times and lows.");
    if (!s_fu.allowed) {
        ImGui::Spacing();
        ImGui::TextColored(g_pal.warn, "Frame capture needs administrator rights: Windows only lets administrators (or members of "
                                       "the Performance Log Users group) start an ETW trace session.");
        ImGui::TextColored(g_pal.warn, "Restart EzPcBench as administrator to use it.");
    }
    const u8 state = g_capture.state.load(std::memory_order_acquire);
    draw_target(state == CAP_RUNNING);
    if (state == CAP_FAILED && !s_fu.status[0]) snprintf(s_fu.status, sizeof(s_fu.status), "%s", g_capture.error);
    if (s_fu.status[0]) {
        ImGui::Spacing();
        const bool bad = state == CAP_FAILED || strncmp(s_fu.status, "Saved", 5) != 0;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(bad ? g_pal.error : g_pal.on_surface_variant, "%s", s_fu.status);
        ImGui::PopTextWrapPos();
    }
    if (state == CAP_DONE) draw_result();
    ImGui::End();
}
