// Main window UI: warnings, suite tree, settings, live panel, results.
#include "app/ui.h"
#include "core/elevation.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include "imgui.h"
#include <windows.h>
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

static const ImVec4 COL_DIM     = { 0.55f, 0.55f, 0.58f, 1.0f };
static const ImVec4 COL_OK      = { 0.40f, 0.85f, 0.45f, 1.0f };
static const ImVec4 COL_RUN     = { 0.95f, 0.80f, 0.25f, 1.0f };
static const ImVec4 COL_BAD     = { 0.95f, 0.35f, 0.30f, 1.0f };
static const ImVec4 COL_WARN    = { 0.95f, 0.60f, 0.20f, 1.0f };
static const ImVec4 COL_WARN_BG = { 0.35f, 0.22f, 0.05f, 1.0f };

void ui_init() {
    for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = !(g_benches[i].flags & BENCH_DEFAULT_OFF);
    s_ui.params.gpu_adapter = sysinfo_default_gpu();
}

bool ui_wants_quit() { return s_ui.quit; }

void ui_start_run() {
    s_ui.status_msg[0] = 0;
    runner_start(s_ui.selected, &s_ui.params);
}

void ui_apply_style(f32 scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.FontSizeBase = 16.0f;
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ChildRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

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

static void banner(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_WARN_BG);
    const f32 h = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f + ImGui::GetStyle().WindowPadding.y;
    ImGui::BeginChild(text, ImVec2(0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(COL_WARN, "%s", text);
}

static void draw_warnings() {
    const SysInfo* s = &g_sys;
    if (!s->elevated) {
        banner("Not running as administrator: disk test file goes to a subfolder, some tweaks are unavailable.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Restart as admin") && !runner_is_running()) {
            if (relaunch_elevated(L"")) s_ui.quit = true;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!s->power.on_ac) {
        banner("Running on battery: results will be lower and less stable. Plug in the charger.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!power_is_best_performance(&s->power)) {
        char text[200];
        snprintf(text, sizeof(text), "Power mode is \"%s\" (plan \"%s\"). Set Best performance for maximum results.",
                 power_mode_name(s->power.mode), s->power.scheme_name);
        banner(text);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

static void draw_sysinfo() {
    const SysInfo* s = &g_sys;
    if (!ImGui::CollapsingHeader("System")) return;
    const CpuInfo* c = &s->cpu;
    ImGui::Text("CPU   %s  (%u cores / %u threads)", c->brand, c->cores, c->threads);
    ImGui::Text("      L1D %u KiB, L2 %u KiB per core, L3 %u KiB  |  %s%s%s%s%s", c->l1d_kib, c->l2_kib, c->l3_kib,
                c->avx2 ? "AVX2 " : "", c->fma ? "FMA " : "", c->avx512f ? "AVX-512 " : "", c->sha ? "SHA " : "",
                c->vaes ? "VAES" : "");
    ImGui::Text("RAM   %.1f GiB usable", s->ram.total_bytes / (f64)GiB);
    for (u32 i = 0; i < s->ram.module_count; ++i) {
        const RamModule* m = &s->ram.modules[i];
        ImGui::Text("      %s: %llu GiB %s @ %u MT/s (rated %u)  %s %s", m->locator, m->size_bytes / GiB, m->type,
                    m->configured_mts, m->speed_mts, m->manufacturer, m->part);
    }
    for (u32 i = 0; i < s->gpu_count; ++i)
        ImGui::Text("GPU%u  %s  (%llu MiB VRAM)", i, s->gpus[i].name, s->gpus[i].dedicated_vram / MiB);
    ImGui::Text("OS    %s, %s", s->os, s->elevated ? "administrator" : "standard user");
    ImGui::Text("Power %s%s, plan \"%s\", mode \"%s\"", s->power.on_ac ? "AC" : "battery",
                s->power.has_battery ? "" : " (no battery)", s->power.scheme_name, power_mode_name(s->power.mode));
}

// ---- toolbar --------------------------------------------------------------------

static void save_json() {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    wcscat_s(dir, L"\\results");
    CreateDirectoryW(dir, nullptr);

    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\ezbench_%04d%02d%02d_%02d%02d%02d.json", dir, t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    if (results_write_json(path, &g_run.params)) snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), "Saved %s", path8);
    else snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), "Failed to write %s", path8);
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
    if (ImGui::Button("  Run  ")) ui_start_run();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel")) runner_cancel();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("All")) for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = true;
    ImGui::SameLine();
    if (ImGui::Button("None")) for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = false;
    ImGui::EndDisabled();

    const bool have_results = g_run.total > 0 && !running;
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_results);
    if (ImGui::Button("Save JSON")) save_json();
    ImGui::SameLine();
    if (ImGui::Button("Copy text")) copy_text();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%u selected", selected);
    if (s_ui.status_msg[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s", s_ui.status_msg);
    }

    // Overall progress.
    if (g_run.total > 0) {
        const f32 p = runner_overall_progress();
        const f64 elapsed = (running ? timer_now() : g_run.end_time) - g_run.start_time;
        char overlay[96];
        snprintf(overlay, sizeof(overlay), "%u / %u tests   %.0f%%   %.1f s", g_run.done_count.load(), g_run.total,
                 p * 100.0f, elapsed);
        ImGui::ProgressBar(p, ImVec2(-FLT_MIN, 0), overlay);
    }
}

// ---- tree ---------------------------------------------------------------------

static void draw_test_row(u32 id, bool running) {
    const BenchDesc* b = &g_benches[id];
    const BenchStatus st = status_of(id);
    ImGui::PushID((int)id);
    ImGui::BeginDisabled(running);
    ImGui::Checkbox(b->name, &s_ui.selected[id]);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", b->cli_id);

    ImGui::SameLine(s_status_x);
    if (st == ST_RUNNING) {
        char overlay[48];
        snprintf(overlay, sizeof(overlay), "%.0f%%", g_live.progress.load() * 100.0f);
        ImGui::ProgressBar(g_live.progress.load(), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), overlay);
    } else if (st == ST_DONE) {
        char summary[64];
        result_summary(id, summary, sizeof(summary));
        ImGui::TextColored(COL_OK, "%s", summary);
    } else if (st != ST_NOT_RUN) {
        ImGui::TextColored(status_color(st), "%s", status_name(st));
        if ((st == ST_FAILED || st == ST_SKIPPED) && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_results[id].error);
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
        const bool open = ImGui::TreeNodeEx("##suite", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed |
                                                           ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth,
                                            "%s", g_suites[s].name);
        ImGui::SameLine(s_status_x);
        bool all = sel == total;
        ImGui::BeginDisabled(running);
        if (ImGui::Checkbox("##all", &all))
            for (u32 i = 0; i < g_bench_count; ++i)
                if (g_benches[i].suite == s) s_ui.selected[i] = all;
        ImGui::EndDisabled();
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
    if (!ImGui::CollapsingHeader("Settings")) return;
    BenchParams* p = &s_ui.params;
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
    ImGui::Checkbox("GPU fullscreen (borderless)", &p->gpu_fullscreen);

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
    ImGui::Checkbox("Disk write-through (FUA)", &p->disk_write_through);

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
    ImGui::Text("%s / %s / %s", g_suites[b->suite].name, b->group, b->name);
    ImGui::TextDisabled("%s", g_live.phase.load());

    char v[32];
    format_value(v, sizeof(v), g_live.current.load(std::memory_order_relaxed));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
    ImGui::Text("%s %s", v, g_live.unit.load());
    ImGui::PopFont();

    const f32 p = g_live.progress.load();
    ImGui::ProgressBar(p, ImVec2(-FLT_MIN, 0));

    // Sparkline: last LIVE_HISTORY samples, oldest first.
    static f32 hist[LIVE_HISTORY];
    const u32 count = g_live.history_count.load(std::memory_order_acquire);
    const u32 n = count < LIVE_HISTORY ? count : LIVE_HISTORY;
    f32 max_v = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        hist[i] = g_live.history[(count - n + i) % LIVE_HISTORY].load(std::memory_order_relaxed);
        if (hist[i] > max_v) max_v = hist[i];
    }
    if (n > 0)
        ImGui::PlotLines("##hist", hist, (int)n, 0, nullptr, 0.0f, max_v * 1.1f,
                         ImVec2(-FLT_MIN, ImGui::GetFontSize() * 6.0f));
}

// ---- results ----------------------------------------------------------------------

static void draw_results() {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("results", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 2.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();

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
        for (u32 k = 0; k < r->count; ++k) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (k == 0) ImGui::Text("%s / %s", g_suites[b->suite].name, b->name);
            ImGui::TableNextColumn();
            if (r->kind == RESULT_SERIES) {
                format_x(x, sizeof(x), m[k].x, r->x_unit);
                snprintf(label, sizeof(label), "%s @ %s", m[k].name, x);
            } else if (r->kind == RESULT_MATRIX && r->cols) {
                snprintf(label, sizeof(label), "%s [%u,%u]", m[k].name, k / r->cols, k % r->cols);
            } else {
                snprintf(label, sizeof(label), "%s", m[k].name);
            }
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            format_value(v, sizeof(v), m[k].value);
            ImGui::TextUnformatted(v);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", m[k].unit);
        }
    }
    ImGui::EndTable();
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
    ImGui::Separator();

    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, -FLT_MIN))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::BeginChild("tree", ImVec2(0, 0));
        draw_tree();
        ImGui::Spacing();
        draw_settings();
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("live", ImVec2(0, ImGui::GetFontSize() * 13.0f), ImGuiChildFlags_Borders);
        draw_live();
        ImGui::EndChild();
        draw_results();
        ImGui::EndTable();
    }
    ImGui::End();
}
