// Main window UI: warnings, suite tree, settings, live panel, results.
#include "app/ui.h"
#include "app/settings.h"
#include "app/theme.h"
#include "core/baseline.h"
#include "core/elevation.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include "imgui.h"
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct UiState {
    bool selected[MAX_BENCHES];
    BenchParams params;
    char status_msg[300];
    f64 last_power_refresh;
    bool quit;
};
static UiState s_ui;

#define COL_DIM  g_pal.on_surface_variant
#define COL_OK   g_pal.ok
#define COL_RUN  g_pal.primary
#define COL_BAD  g_pal.error
#define COL_WARN g_pal.warn

void ui_init() {
    for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = !(g_benches[i].flags & BENCH_DEFAULT_OFF);
    s_ui.params.gpu_adapter = sysinfo_default_gpu();
    settings_load(&s_ui.params, s_ui.selected);
}

void ui_shutdown() { settings_flush(&s_ui.params, s_ui.selected); }

bool ui_wants_quit() { return s_ui.quit; }

void ui_set_duration(u32 ms) {
    if (ms >= 100) s_ui.params.duration_ms = ms;
}

void ui_start_run() {
    s_ui.status_msg[0] = 0;
    runner_start(s_ui.selected, &s_ui.params);
}

void ui_apply_style(f32 scale) { theme_apply_style(scale); }

static ImVec4 status_color(BenchStatus st) {
    switch (st) {
        case ST_RUNNING: return COL_RUN;
        case ST_DONE: return COL_OK;
        case ST_FAILED: return COL_BAD;
        case ST_SKIPPED: case ST_CANCELLED: return COL_WARN;
        default: return COL_DIM;
    }
}

static f32 s_status_x;  // window-local x of the status column in the tree

static BenchStatus status_of(u32 id) { return (BenchStatus)g_status[id].load(std::memory_order_acquire); }

// Short summary of a finished result for the tree ("12.3 GB/s", "15 points").
static void result_summary(u32 id, char* buf, u32 size) {
    const BenchResult* r = &g_results[id];
    buf[0] = 0;
    if (r->count == 0) return;
    const Metric* m = res_metrics(r);
    if (r->kind == RESULT_VALUES) {
        char v[32];
        format_value(v, sizeof(v), m[0].value);
        snprintf(buf, size, "%s %s", v, m[0].unit);
    } else if (r->kind == RESULT_SERIES) {
        snprintf(buf, size, "%u points", r->count);
    } else {
        snprintf(buf, size, "%ux%u matrix", r->rows, r->cols);
    }
}

// ---- banners ------------------------------------------------------------------

static void draw_warnings() {
    const SysInfo* s = &g_sys;
    const char* lines[6];
    u32 n = 0;
    i32 admin_line = -1;
    if (!s->elevated) {
        admin_line = (i32)n;
        lines[n++] = "Not running as administrator: disk test file goes to a subfolder, some tweaks are unavailable.";
    }
    if (!s->power.on_ac) lines[n++] = "Running on battery: results will be lower and less stable. Plug in the charger.";
    static char power[200];
    if (!power_is_best_performance(&s->power)) {
        snprintf(power, sizeof(power), "Power mode is \"%s\" (plan \"%s\"). Set Best performance for maximum results.",
                 power_mode_name(s->power.mode), s->power.scheme_name);
        lines[n++] = power;
    }
    bool disk_selected = false;
    for (u32 i = 0; i < g_bench_count; ++i) disk_selected |= g_benches[i].suite == SUITE_DISK && s_ui.selected[i];
    static char drive[2][192];
    const u32 nd = disk_selected ? drive_target_warnings(s_ui.params.disk_drive, drive) : 0;
    for (u32 i = 0; i < nd; ++i) lines[n++] = drive[i];
    if (theme_warnings(lines, n, admin_line, "Restart as admin") && !runner_is_running() && relaunch_elevated(L""))
        s_ui.quit = true;
}

// Dimmed label in a fixed column, then the value ("" label = continuation line).
static void sys_line(const char* label, const char* fmt, ...) {
    const f32 x = ImGui::GetCursorPosX();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(x + ImGui::GetFontSize() * 4.0f);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

static void draw_sysinfo() {
    const SysInfo* s = &g_sys;
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const bool open = ImGui::CollapsingHeader("System");
    ImGui::PopFont();
    if (!open) return;
    ImGui::Indent(ImGui::GetStyle().FramePadding.x);
    const CpuInfo* c = &s->cpu;
    sys_line("CPU", "%s  (%u cores / %u threads)", c->brand, c->cores, c->threads);
    sys_line("", "L1D %u KiB, L2 %u KiB per core, L3 %u KiB  \xC2\xB7  %s%s%s%s%s", c->l1d_kib, c->l2_kib, c->l3_kib,
             c->avx2 ? "AVX2 " : "", c->fma ? "FMA " : "", c->avx512f ? "AVX-512 " : "", c->sha ? "SHA " : "",
             c->vaes ? "VAES" : "");
    sys_line("RAM", "%.1f GiB usable", s->ram.total_bytes / (f64)GiB);
    for (u32 i = 0; i < s->ram.module_count; ++i) {
        const RamModule* m = &s->ram.modules[i];
        sys_line("", "%s: %llu GiB %s @ %u MT/s (rated %u)  %s %s", m->locator, m->size_bytes / GiB, m->type,
                 m->configured_mts, m->speed_mts, m->manufacturer, m->part);
    }
    for (u32 i = 0; i < s->gpu_count; ++i)
        sys_line(i ? "" : "GPU", "%u: %s  (%llu MiB VRAM)", i, s->gpus[i].name, s->gpus[i].dedicated_vram / MiB);
    for (u32 i = 0; i < s->drive_count; ++i) {
        char d[192];
        drive_describe(&s->drives[i], d, sizeof(d));
        sys_line(i ? "" : "Disk", "%c:  %s", s->drives[i].letter, d);
    }
    sys_line("OS", "%s, %s", s->os, s->elevated ? "administrator" : "standard user");
    sys_line("Power", "%s%s, plan \"%s\", mode \"%s\"", s->power.on_ac ? "AC" : "battery",
             s->power.has_battery ? "" : " (no battery)", s->power.scheme_name, power_mode_name(s->power.mode));
    ImGui::Unindent(ImGui::GetStyle().FramePadding.x);
    ImGui::Spacing();
}

// ---- toolbar --------------------------------------------------------------------

// "results" folder next to the exe (created on demand).
static void results_dir(wchar_t* dir) {
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    wcscat_s(dir, MAX_PATH, L"\\results");
    CreateDirectoryW(dir, nullptr);
}

static void save_results(bool csv) {
    wchar_t dir[MAX_PATH];
    results_dir(dir);
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\ezbench_%04d%02d%02d_%02d%02d%02d.%s", dir, t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, csv ? L"csv" : L"json");
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    const bool ok = csv ? results_write_csv(path, &g_run.params) : results_write_json(path, &g_run.params);
    snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), ok ? "Saved %s" : "Failed to write %s", path8);
}

static void compare_dialog() {
    wchar_t dir[MAX_PATH], path[MAX_PATH] = {};
    results_dir(dir);
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
    ofn.lpstrFilter = L"ez-pc-bench results (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir;
    ofn.lpstrTitle = L"Compare against saved results";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool picked = GetOpenFileNameW(&ofn) != 0;
    if (SUCCEEDED(co)) CoUninitialize();
    if (!picked) return;
    char err[160];
    if (baseline_load(path, err, sizeof(err)))
        snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), "Baseline: %u metrics from %s", g_baseline.count, g_baseline.file);
    else
        snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), "Cannot load baseline: %s", err);
}

static void copy_text() {
    static char buf[256 * 1024];
    results_format_text(buf, sizeof(buf), &g_run.params);
    ImGui::SetClipboardText(buf);
    snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), "Results copied to clipboard");
}

static void draw_toolbar() {
    const bool running = runner_is_running();
    u32 selected = 0;
    for (u32 i = 0; i < g_bench_count; ++i) selected += s_ui.selected[i];

    ImGui::BeginDisabled(running || selected == 0);
    if (theme_button_primary("Run")) ui_start_run();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (theme_button("Cancel")) runner_cancel();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (theme_button("All")) for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = true;
    ImGui::SameLine();
    if (theme_button("None")) for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = false;
    ImGui::EndDisabled();

    const bool have_results = g_run.total > 0 && !running;
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_results);
    if (theme_button("Save JSON")) save_results(false);
    ImGui::SameLine();
    if (theme_button("Save CSV")) save_results(true);
    ImGui::SameLine();
    if (theme_button("Copy text")) copy_text();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running);      // the file dialog would load shell extensions mid-benchmark
    if (theme_button("Compare...")) compare_dialog();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%u selected", selected);
    if (s_ui.status_msg[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", s_ui.status_msg);
    }

    // Overall progress: thin bar with a caption underneath.
    if (g_run.total > 0) {
        const f32 p = runner_overall_progress();
        const f64 elapsed = (running ? timer_now() : g_run.end_time) - g_run.start_time;
        theme_progress(p, ImGui::GetFontSize() * 0.35f);
        ImGui::TextDisabled("%u / %u tests  \xC2\xB7  %.0f%%  \xC2\xB7  %.1f s", g_run.done_count.load(), g_run.total,
                            p * 100.0f, elapsed);
    }
}

// ---- tree ---------------------------------------------------------------------

static void draw_test_row(u32 id, bool running) {
    const BenchDesc* b = &g_benches[id];
    const BenchStatus st = status_of(id);
    ImGui::PushID((int)id);
    // Selection is frozen during a run, but the tree stays at full contrast (clicks are ignored).
    bool sel = s_ui.selected[id];
    if (theme_checkbox(b->name, &sel) && !running) s_ui.selected[id] = sel;
    const f32 row_h = ImGui::GetItemRectSize().y, row_top = ImGui::GetItemRectMin().y;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b->cli_id);

    ImGui::SameLine(s_status_x);
    if (st == ST_RUNNING) {
        // Thin bar with the percentage beside it, both centered on the row.
        const f32 p = g_live.progress.load();
        const f32 y = ImGui::GetCursorPosY(), h = ImGui::GetFontSize() * 0.3f;
        const f32 text_w = ImGui::CalcTextSize("100%").x;
        ImGui::SetCursorPosY(y + (row_h - h) * 0.5f);
        theme_progress(p, h, ImGui::GetContentRegionAvail().x - text_w - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        char pct[8];
        snprintf(pct, sizeof(pct), "%.0f%%", p * 100.0f);
        const ImVec2 at(ImGui::GetCursorScreenPos().x, row_top + (row_h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), pct);
        ImGui::Dummy(ImVec2(text_w, h));
    } else if (st == ST_DONE) {
        char summary[64];
        result_summary(id, summary, sizeof(summary));
        theme_push_numbers();
        ImGui::TextColored(COL_OK, "%s", summary);
        ImGui::PopFont();
    } else if (st != ST_NOT_RUN) {
        ImGui::TextColored(status_color(st), "%s", status_name(st));
        if ((st == ST_FAILED || st == ST_SKIPPED) && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_results[id].error);
    } else {
        ImGui::TextUnformatted("");     // end the line started by SameLine
    }
    ImGui::PopID();
}

static void draw_tree() {
    const bool running = runner_is_running();
    s_status_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.55f;
    for (u32 s = 0; s < SUITE_COUNT; ++s) {
        u32 total = 0, sel = 0, in_run = 0, done = 0;
        for (u32 i = 0; i < g_bench_count; ++i) {
            if (g_benches[i].suite != s) continue;
            ++total;
            sel += s_ui.selected[i];
            BenchStatus st = status_of(i);
            in_run += st != ST_NOT_RUN;
            done += st != ST_NOT_RUN && st != ST_PENDING && st != ST_RUNNING;
        }
        if (total == 0) continue;

        ImGui::PushID((int)s);
        ImGui::PushFont(g_fonts.medium, 0.0f);
        const bool open = ImGui::TreeNodeEx("##suite", (sel ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_Framed |
                                                           ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth,
                                            "%s", g_suites[s].name);
        ImGui::PopFont();
        const f32 header_h = ImGui::GetItemRectSize().y;
        ImGui::SameLine(s_status_x);
        // The header is a full frame tall; center the smaller checkbox (and the count after it) on it.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (header_h - theme_checkbox_height()) * 0.5f);
        bool all = sel == total;
        if (theme_checkbox("##all", &all) && !running)
            for (u32 i = 0; i < g_bench_count; ++i)
                if (g_benches[i].suite == s) s_ui.selected[i] = all;
        ImGui::SameLine();
        if (running && in_run) ImGui::TextDisabled("%u/%u done", done, in_run);
        else ImGui::TextDisabled("%u/%u selected", sel, total);

        if (open) {
            const char* group = nullptr;
            bool group_open = false;
            for (u32 i = 0; i < g_bench_count; ++i) {
                if (g_benches[i].suite != s) continue;
                if (!group || strcmp(group, g_benches[i].group) != 0) {
                    if (group_open) ImGui::TreePop();
                    group = g_benches[i].group;
                    group_open = ImGui::TreeNodeEx(group, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                }
                if (group_open) draw_test_row(i, running);
            }
            if (group_open) ImGui::TreePop();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// ---- settings -------------------------------------------------------------------

static void draw_settings() {
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const bool open = ImGui::CollapsingHeader("Settings");
    ImGui::PopFont();
    if (!open) return;
    BenchParams* p = &s_ui.params;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);
    static const char* const modes[] = { "System", "Light", "Dark" };
    int mode = (int)theme_mode();
    if (ImGui::Combo("Theme", &mode, modes, (int)ARRAY_COUNT(modes))) theme_set_mode((ThemeMode)mode);
    ImGui::PopItemWidth();
    ImGui::BeginDisabled(runner_is_running());
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);

    int duration = (int)p->duration_ms;
    if (ImGui::SliderInt("Duration per phase (ms)", &duration, 500, 20000)) p->duration_ms = (u32)duration;
    int threads = (int)p->thread_count;
    if (ImGui::SliderInt("Threads (0 = all)", &threads, 0, (int)g_sys.cpu.threads)) p->thread_count = (u32)threads;

    if (g_sys.gpu_count > 0) {
        if (p->gpu_adapter >= g_sys.gpu_count) p->gpu_adapter = 0;
        if (ImGui::BeginCombo("GPU", g_sys.gpus[p->gpu_adapter].name)) {
            for (u32 i = 0; i < g_sys.gpu_count; ++i)
                if (ImGui::Selectable(g_sys.gpus[i].name, i == p->gpu_adapter)) p->gpu_adapter = i;
            ImGui::EndCombo();
        }
    }
    int res[2] = { (int)p->gpu_width, (int)p->gpu_height };
    if (ImGui::InputInt2("GPU resolution", res)) {
        p->gpu_width = (u32)(res[0] < 64 ? 64 : res[0]);
        p->gpu_height = (u32)(res[1] < 64 ? 64 : res[1]);
    }
    theme_checkbox("GPU fullscreen (borderless)", &p->gpu_fullscreen);

    char drive_label[8];
    snprintf(drive_label, sizeof(drive_label), "%c:", (char)p->disk_drive);
    if (ImGui::BeginCombo("Disk target", drive_label)) {
        const DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; ++d) {
            if (!(drives & (1u << d))) continue;
            wchar_t root[4] = { (wchar_t)(L'A' + d), L':', L'\\', 0 };
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            char label[8];
            snprintf(label, sizeof(label), "%c:", 'A' + d);
            if (ImGui::Selectable(label, p->disk_drive == (wchar_t)(L'A' + d))) p->disk_drive = (wchar_t)(L'A' + d);
        }
        ImGui::EndCombo();
    }
    static const u32 sizes_mib[] = { 64, 256, 512, 1024, 2048, 4096, 8192 };
    char size_label[16];
    snprintf(size_label, sizeof(size_label), "%llu MiB", p->disk_file_bytes / MiB);
    if (ImGui::BeginCombo("Disk test file", size_label)) {
        for (u32 i = 0; i < ARRAY_COUNT(sizes_mib); ++i) {
            snprintf(size_label, sizeof(size_label), "%u MiB", sizes_mib[i]);
            if (ImGui::Selectable(size_label, p->disk_file_bytes == sizes_mib[i] * MiB)) p->disk_file_bytes = sizes_mib[i] * MiB;
        }
        ImGui::EndCombo();
    }
    int passes = (int)p->disk_passes;
    if (ImGui::SliderInt("Disk passes", &passes, 1, 9)) p->disk_passes = (u32)passes;
    theme_checkbox("Disk write-through (FUA)", &p->disk_write_through);

    ImGui::PopItemWidth();
    ImGui::EndDisabled();
}

// ---- live panel -----------------------------------------------------------------

static void draw_live() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    if (!runner_is_running() || cur < 0) {
        if (g_run.total == 0) ImGui::TextDisabled("Select tests on the left and press Run.");
        else ImGui::TextDisabled("Run finished in %.1f s.", g_run.end_time - g_run.start_time);
        return;
    }
    const BenchDesc* b = &g_benches[cur];
    ImGui::TextDisabled("%s  \xC2\xB7  %s", g_suites[b->suite].name, b->group);
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.25f));
    ImGui::TextUnformatted(b->name);
    ImGui::PopFont();

    char v[32];
    format_value(v, sizeof(v), g_live.current.load(std::memory_order_relaxed));
    // Big value with the unit sitting on its baseline.
    const f32 y0 = ImGui::GetCursorPosY();
    ImGui::PushFont(g_fonts.medium, theme_font_size(2.6f));
    ImGui::TextUnformatted(v);
    const f32 big_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(y0 + big_h * 0.78f - ImGui::GetTextLineHeight() * 0.78f);
    ImGui::TextDisabled("%s", g_live.unit.load());
    ImGui::SetCursorPosY(y0 + big_h + ImGui::GetStyle().ItemSpacing.y);

    const f32 p = g_live.progress.load();
    theme_progress(p, ImGui::GetFontSize() * 0.35f);
    ImGui::TextDisabled("%s  \xC2\xB7  %.0f%%", g_live.phase.load(), p * 100.0f);

    // Sparkline: last LIVE_HISTORY samples, oldest first.
    static f32 hist[LIVE_HISTORY];
    const u32 count = g_live.history_count.load(std::memory_order_acquire);
    const u32 n = count < LIVE_HISTORY ? count : LIVE_HISTORY;
    f32 max_v = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        hist[i] = g_live.history[(count - n + i) % LIVE_HISTORY].load(std::memory_order_relaxed);
        if (hist[i] > max_v) max_v = hist[i];
    }
    if (n > 0) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
        ImGui::PlotLines("##hist", hist, (int)n, 0, nullptr, 0.0f, max_v * 1.1f, ImVec2(-FLT_MIN, -FLT_MIN));
        ImGui::PopStyleColor();
    }
}

// ---- results ----------------------------------------------------------------------

// Moves the cursor so an item of width `w` ends at the right edge of the current cell.
static void align_right(f32 w) {
    const f32 avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
}

static void text_right(const char* s) {
    align_right(ImGui::CalcTextSize(s).x);
    ImGui::TextUnformatted(s);
}

// Baseline value + colored change for one row (the two compare columns).
static void compare_cells(const char* base_text, f64 value, f64 base, const char* unit) {
    ImGui::TableNextColumn();
    theme_push_numbers();
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DIM);
    text_right(base_text);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    const f64 pct = delta_pct(value, base);
    const i32 rating = delta_rating(pct, unit);
    char d[16];
    format_delta(d, sizeof(d), pct);
    ImGui::PushStyleColor(ImGuiCol_Text, rating > 0 ? COL_OK : rating < 0 ? COL_BAD : COL_DIM);
    text_right(d);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void draw_results() {
    // Material data table: horizontal dividers only, quiet header. Two more columns with a baseline loaded.
    const bool cmp = g_baseline.loaded;
    const int cols = cmp ? 6 : 4;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(cmp ? "results_cmp" : "results", cols, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 2.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    if (cmp) {
        ImGui::TableSetupColumn("Baseline", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("\xCE\x94", ImGuiTableColumnFlags_WidthStretch, 0.8f);     // Δ
    }
    // Header row by hand so the number columns can be right-aligned over their numbers.
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < cols; ++c) {
        ImGui::TableSetColumnIndex(c);
        const char* name = ImGui::TableGetColumnName(c);
        if (c == 2 || c >= 4) align_right(ImGui::CalcTextSize(name).x);
        ImGui::TableHeader(name);
    }
    ImGui::PopFont();
    ImGui::PopStyleColor();

    char v[32], x[32], label[96];
    for (u32 i = 0; i < g_bench_count; ++i) {
        const BenchStatus st = status_of(i);
        if (st == ST_NOT_RUN || st == ST_PENDING || st == ST_RUNNING) continue;
        const BenchDesc* b = &g_benches[i];
        const BenchResult* r = &g_results[i];
        if (st != ST_DONE) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s / %s", g_suites[b->suite].name, b->name);
            ImGui::TableNextColumn(); ImGui::TextColored(status_color(st), "%s", status_name(st));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r->error);
            ImGui::TableNextColumn();
            continue;
        }
        const Metric* m = res_metrics(r);
        if (r->kind == RESULT_MATRIX && r->count) {
            // One summary row; the full grid is on the Charts tab.
            f64 lo, avg, hi;
            res_matrix_stats(r, &lo, &avg, &hi);
            char vmin[32], vavg[32], vmax[32], triple[100];
            format_value(vmin, sizeof(vmin), lo);
            format_value(vavg, sizeof(vavg), avg);
            format_value(vmax, sizeof(vmax), hi);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s / %s", g_suites[b->suite].name, b->name);
            ImGui::TableNextColumn(); ImGui::Text("%s min / avg / max (%ux%u)", m[0].name, r->rows, r->cols);
            ImGui::TableNextColumn();
            snprintf(triple, sizeof(triple), "%s / %s / %s", vmin, vavg, vmax);
            theme_push_numbers();
            text_right(triple);
            ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", m[0].unit);
            f64 blo, bavg, bhi;
            if (cmp && baseline_matrix(b->cli_id, r, &blo, &bavg, &bhi)) {     // change of the average
                format_value(vmin, sizeof(vmin), blo);
                format_value(vavg, sizeof(vavg), bavg);
                format_value(vmax, sizeof(vmax), bhi);
                snprintf(triple, sizeof(triple), "%s / %s / %s", vmin, vavg, vmax);
                compare_cells(triple, avg, bavg, m[0].unit);
            }
            continue;
        }
        for (u32 k = 0; k < r->count; ++k) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (k == 0) ImGui::Text("%s / %s", g_suites[b->suite].name, b->name);
            ImGui::TableNextColumn();
            if (r->kind == RESULT_SERIES) {
                format_x(x, sizeof(x), m[k].x, r->x_unit);
                snprintf(label, sizeof(label), "%s @ %s", m[k].name, x);
            } else {
                snprintf(label, sizeof(label), "%s", m[k].name);
            }
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            format_value(v, sizeof(v), m[k].value);
            theme_push_numbers();
            text_right(v);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", m[k].unit);
            f64 base;
            if (cmp && baseline_metric(b->cli_id, r, k, &base)) {
                format_value(v, sizeof(v), base);
                compare_cells(v, m[k].value, base, m[k].unit);
            }
        }
    }
    ImGui::EndTable();
}

// ---- charts: series plots and matrix grids ----------------------------------------------

static void draw_series(const BenchResult* r) {
    static f32 ys[1024];
    const Metric* m = res_metrics(r);
    const u32 n = r->count < 1024 ? r->count : 1024;
    f32 hi = 0.0f;
    for (u32 k = 0; k < n; ++k) {
        ys[k] = (f32)m[k].value;
        if (ys[k] > hi) hi = ys[k];
    }
    char first[32], last[32];
    format_x(first, sizeof(first), m[0].x, r->x_unit);
    format_x(last, sizeof(last), m[n - 1].x, r->x_unit);
    char overlay[96];
    snprintf(overlay, sizeof(overlay), "%s [%s] vs %s (%s .. %s)", m[0].name, m[0].unit, r->x_name ? r->x_name : "x",
             first, last);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
    ImGui::PlotLines("##series", ys, (int)n, 0, overlay, 0.0f, hi * 1.15f, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 8.0f));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        // Map mouse x to a point index (PlotLines spaces points evenly).
        const f32 t = (ImGui::GetIO().MousePos.x - ImGui::GetItemRectMin().x) / ImGui::GetItemRectSize().x;
        u32 k = (u32)(t * (n - 1) + 0.5f);
        if (k >= n) k = n - 1;
        char x[32], v[32];
        format_x(x, sizeof(x), m[k].x, r->x_unit);
        format_value(v, sizeof(v), m[k].value);
        ImGui::SetTooltip("%s: %s %s", x, v, m[k].unit);
    }
}

static void draw_matrix(const BenchResult* r) {
    const Metric* m = res_metrics(r);
    f64 lo = 0.0, hi = 0.0;
    bool first = true;
    for (u32 k = 0; k < r->count; ++k) {
        if (r->rows == r->cols && k / r->cols == k % r->cols) continue;
        lo = first || m[k].value < lo ? m[k].value : lo;
        hi = first || m[k].value > hi ? m[k].value : hi;
        first = false;
    }
    ImGui::TextDisabled("%s [%s], green = low, red = high", m[0].name, m[0].unit);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    const bool whole = lo >= 10.0;      // e.g. 16x16 latencies in ns: integers keep the grid narrow
    theme_push_numbers();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetFontSize() * 0.3f, ImGui::GetFontSize() * 0.15f));
    if (!ImGui::BeginTable("matrix", r->cols + 1, flags)) { ImGui::PopStyleVar(); ImGui::PopFont(); return; }
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableNextColumn();
    for (u32 c = 0; c < r->cols; ++c) { ImGui::TableNextColumn(); ImGui::TextDisabled("%u", c); }
    char v[32];
    for (u32 row = 0; row < r->rows; ++row) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%u", row);
        for (u32 c = 0; c < r->cols; ++c) {
            ImGui::TableNextColumn();
            const u32 k = row * r->cols + c;
            if (k >= r->count || (r->rows == r->cols && row == c)) continue;
            const f32 t = hi > lo ? (f32)((m[k].value - lo) / (hi - lo)) : 0.0f;
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, theme_heat(t));
            if (whole) snprintf(v, sizeof(v), "%.0f", m[k].value);
            else format_value(v, sizeof(v), m[k].value);
            ImGui::TextUnformatted(v);
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

static void draw_charts() {
    bool any = false;
    for (u32 i = 0; i < g_bench_count; ++i) {
        if (status_of(i) != ST_DONE) continue;
        const BenchResult* r = &g_results[i];
        if (r->count == 0 || r->kind == RESULT_VALUES) continue;
        any = true;
        const BenchDesc* b = &g_benches[i];
        ImGui::PushID((int)i);
        if (ImGui::TreeNodeEx("##chart", ImGuiTreeNodeFlags_DefaultOpen, "%s / %s", g_suites[b->suite].name, b->name)) {
            if (r->kind == RESULT_SERIES) draw_series(r);
            else draw_matrix(r);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("Series and matrix results (latency curves, scaling, core-to-core) appear here.");
}

// ---- frame --------------------------------------------------------------------------

void ui_draw() {
    if (!runner_is_running() && timer_now() - s_ui.last_power_refresh > 2.0) {
        sysinfo_refresh_power();
        s_ui.last_power_refresh = timer_now();
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_warnings();
    draw_sysinfo();
    draw_toolbar();
    ImGui::Spacing();

    // Invisible but draggable column divider: tables cache border colors in BeginTable, so push around it only.
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0, 0, 0, 0));
    const bool layout = ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                                          ImVec2(0, -FLT_MIN));
    ImGui::PopStyleColor();
    if (layout) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        theme_begin_panel("tree", ImVec2(0, 0));
        draw_tree();
        ImGui::Spacing();
        draw_settings();
        theme_end_panel();

        ImGui::TableNextColumn();
        theme_begin_panel("live", ImVec2(0, ImGui::GetFontSize() * 14.0f), false);
        draw_live();
        theme_end_panel();
        ImGui::Spacing();
        theme_begin_panel("output", ImVec2(0, 0), false);
        // Segmented tabs.
        static int tab = 0;
        if (theme_tab("Results", tab == 0)) tab = 0;
        ImGui::SameLine(0.0f, 4.0f);
        if (theme_tab("Charts", tab == 1)) tab = 1;
        if (g_baseline.loaded) {
            // Baseline file at the right end of the tab row, with a quiet button to drop it.
            char label[400];
            snprintf(label, sizeof(label), "vs %s  \xC2\xB7  %s", g_baseline.file, g_baseline.timestamp);
            const f32 clear_w = ImGui::CalcTextSize("Clear").x + ImGui::GetFontSize() * 2.0f;
            ImGui::SameLine();
            const f32 avail = ImGui::GetContentRegionAvail().x - clear_w;
            const f32 label_w = ImGui::CalcTextSize(label).x;
            ImGui::AlignTextToFramePadding();
            if (avail > label_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - label_w);
            ImGui::TextDisabled("%s", label);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Baseline: %u metrics. Green = better, red = worse (> 1%%).", g_baseline.count);
            ImGui::SameLine();
            if (theme_tab("Clear", false)) baseline_clear();
        }
        if (tab == 0) {
            draw_results();
        } else {
            ImGui::BeginChild("charts", ImVec2(0, 0));
            draw_charts();
            ImGui::EndChild();
        }
        theme_end_panel();
        ImGui::EndTable();
    }
    ImGui::End();
    if (!runner_is_running()) settings_tick(&s_ui.params, s_ui.selected);
}
