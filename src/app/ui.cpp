// Main window UI: warnings, suite tree, settings, live panel, results.
#include "app/ui.h"
#include "app/settings.h"
#include "app/theme.h"
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
    bool dev;           // --dev: show the smoke-test suite (runner / UI self-tests)
};
static UiState s_ui;

static bool suite_visible(u32 suite) { return suite != SUITE_SMOKE || s_ui.dev; }

void ui_set_dev(bool dev) { s_ui.dev = dev; }

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

void ui_select_only(const char* ids) {
    for (u32 i = 0; i < g_bench_count; ++i) {
        const char* id = g_benches[i].cli_id;
        bool match = false;
        for (const char* p = ids; *p && !match;) {
            const char* end = strchr(p, ',');
            const size_t n = end ? (size_t)(end - p) : strlen(p);
            match = n && strncmp(id, p, n) == 0 && (id[n] == 0 || id[n] == '.');
            p += n + (end ? 1 : 0);
        }
        s_ui.selected[i] = match;
    }
}

void ui_start_run() {
    s_ui.status_msg[0] = 0;
    bool run[MAX_BENCHES];      // hidden suites never run, whatever the saved selection says
    for (u32 i = 0; i < g_bench_count; ++i) run[i] = s_ui.selected[i] && suite_visible(g_benches[i].suite);
    runner_start(run, &s_ui.params);
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
        char v[32], fps[32];
        format_value(v, sizeof(v), m[0].value);
        format_frame_fps(fps, sizeof(fps), m[0].name, m[0].unit, m[0].value);
        snprintf(buf, size, "%s %s%s", v, m[0].unit, fps);
    } else if (r->kind == RESULT_SERIES) {
        snprintf(buf, size, "%u points", r->count);
    } else {
        snprintf(buf, size, "%ux%u matrix", r->rows, r->cols);
    }
}

// ---- banners ------------------------------------------------------------------

static void draw_warnings() {
    const SysInfo* s = &g_sys;
    const char* lines[12];
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
    static char drive[26][2][192];
    for (u32 d = 0; d < 26 && disk_selected; ++d) {
        if (!((s_ui.params.disk_drives >> d) & 1)) continue;
        const u32 nd = drive_target_warnings((wchar_t)(L'A' + d), drive[d]);
        for (u32 i = 0; i < nd && n < ARRAY_COUNT(lines); ++i) lines[n++] = drive[d][i];
    }
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

// Compact buttons for the tool windows, right-aligned inside the "System" header row just drawn.
static void draw_tool_buttons() {
    struct ToolButton { ToolWindow tool; ThemeIcon icon; const char* label; const char* tip; };
    static const ToolButton buttons[] = {
        { TOOL_SETTINGS, ICON_GEAR, nullptr, "Settings: phase duration, threads, GPU resolution, disk test file and passes, theme." },
    };
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const ImVec2 after = ImGui::GetCursorScreenPos();
    const f32 header_h = mx.y - mn.y, h = header_h - ImGui::GetStyle().FramePadding.y;
    const f32 gap = ImGui::GetStyle().ItemSpacing.x * 0.6f;
    f32 total = 0.0f;
    for (u32 i = 0; i < ARRAY_COUNT(buttons); ++i) total += theme_icon_button_width(buttons[i].label, h) + (i ? gap : 0.0f);
    f32 x = mx.x - (header_h - h) * 0.5f - total;
    for (u32 i = 0; i < ARRAY_COUNT(buttons); ++i) {
        const ToolButton* b = &buttons[i];
        ImGui::SetCursorScreenPos(ImVec2(x, mn.y + (header_h - h) * 0.5f));
        ImGui::PushID((int)b->tool);
        if (theme_icon_button("##tool", b->icon, b->label, h, b->tip)) ui_request_tool(b->tool);
        ImGui::PopID();
        x += theme_icon_button_width(b->label, h) + gap;
    }
    ImGui::SetCursorScreenPos(after);
}

static void draw_sysinfo() {
    const SysInfo* s = &g_sys;
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const bool open = ImGui::CollapsingHeader("System", ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopFont();
    draw_tool_buttons();
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

void ui_results_dir(wchar_t* dir) {
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    wcscat_s(dir, MAX_PATH, L"\\results");
    CreateDirectoryW(dir, nullptr);
}

// "Save results" asks for an optional run name; empty = the default name. The name is the file name.
struct SaveDialog {
    bool open_request;
    char name[128];
    char default_name[128];     // ezbench_20261111_113811_r7-250_rtx5060-laptop
};
static SaveDialog s_save;

static void default_run_name(char* out, u32 size) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    const BenchParams* p = &g_run.params;
    char cpu[48], gpu[48];
    cpu_short_name(g_sys.cpu.brand, cpu, sizeof(cpu));
    gpu_short_name(p->gpu_adapter < g_sys.gpu_count ? g_sys.gpus[p->gpu_adapter].name : "", gpu, sizeof(gpu));
    snprintf(out, size, "ezbench_%04d%02d%02d_%02d%02d%02d_%s_%s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec, cpu, gpu);
}

static void save_results(const char* name) {
    // File-name safe: no \ / : * ? " < > | or control characters, no leading / trailing spaces or dots.
    char clean[128];
    u32 n = 0;
    for (const char* c = name; *c && n + 1 < sizeof(clean); ++c)
        clean[n++] = ((u8)*c < 32 || strchr("\\/:*?\"<>|", *c)) ? '_' : *c;
    clean[n] = 0;
    while (n && (clean[n - 1] == ' ' || clean[n - 1] == '.')) clean[--n] = 0;
    const char* start = clean;
    while (*start == ' ' || *start == '.') ++start;
    if (!*start) start = s_save.default_name;

    wchar_t dir[MAX_PATH], wname[128], path[MAX_PATH];
    ui_results_dir(dir);
    MultiByteToWideChar(CP_UTF8, 0, start, -1, wname, 128);
    swprintf(path, MAX_PATH, L"%s\\%s.toon.txt", dir, wname);
    for (u32 k = 2; GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && k < 1000; ++k)     // keep existing files
        swprintf(path, MAX_PATH, L"%s\\%s_%u.toon.txt", dir, wname, k);
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    const bool ok = results_write_toon(path, &g_run.params);
    snprintf(s_ui.status_msg, sizeof(s_ui.status_msg), ok ? "Saved %s" : "Failed to write %s", path8);
}

static void draw_save_dialog() {
    if (s_save.open_request) {
        s_save.open_request = false;
        s_save.name[0] = 0;
        default_run_name(s_save.default_name, sizeof(s_save.default_name));
        ImGui::OpenPopup("Save results");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f));
    if (!ImGui::BeginPopupModal("Save results", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextDisabled("Run name (optional)");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool save = ImGui::InputTextWithHint("##name", s_save.default_name, s_save.name, sizeof(s_save.name),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("Enter saves with the name shown (results\\<name>.toon.txt next to the exe).");
    ImGui::Spacing();
    save |= theme_button_primary("Save");
    ImGui::SameLine();
    const bool cancel = theme_button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (save) {
        save_results(s_save.name[0] ? s_save.name : s_save.default_name);
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static u32 s_tool_requests;      // ToolWindow bits; main_gui opens / focuses them after the frame

void ui_request_tool(ToolWindow t) { s_tool_requests |= 1u << t; }

u32 ui_take_tool_requests() {
    const u32 r = s_tool_requests;
    s_tool_requests = 0;
    return r;
}

BenchParams* ui_params() { return &s_ui.params; }

// What to test on: GPU for both GPU suites (one adapter per run) and the disk(s) for the disk suite.
static void draw_targets() {
    BenchParams* p = &s_ui.params;
    ImGui::BeginDisabled(runner_is_running());
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("GPU");
    ImGui::SameLine();
    if (g_sys.gpu_count > 0) {
        if (p->gpu_adapter >= g_sys.gpu_count) p->gpu_adapter = 0;
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(g_sys.gpus[p->gpu_adapter].name).x + ImGui::GetFrameHeight() * 2.0f);
        if (ImGui::BeginCombo("##gpu", g_sys.gpus[p->gpu_adapter].name)) {
            for (u32 i = 0; i < g_sys.gpu_count; ++i) {
                char label[192];
                snprintf(label, sizeof(label), "%s  (%llu MiB)", g_sys.gpus[i].name, g_sys.gpus[i].dedicated_vram / MiB);
                if (ImGui::Selectable(label, i == p->gpu_adapter)) p->gpu_adapter = i;
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("none found");
    }

    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
    ImGui::TextDisabled("Disks");
    for (u32 i = 0; i < g_sys.drive_count; ++i) {
        const DriveInfo* d = &g_sys.drives[i];
        const u32 bit = 1u << (d->letter - 'A');
        char label[8];
        snprintf(label, sizeof(label), "%c:", d->letter);
        ImGui::SameLine(0.0f, i ? 4.0f : -1.0f);
        ImGui::PushID((int)i);
        if (theme_tab(label, (p->disk_drives & bit) != 0)) {
            const u32 next = p->disk_drives ^ bit;
            if (next) p->disk_drives = next;    // at least one drive stays selected
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            char desc[192];
            drive_describe(d, desc, sizeof(desc));
            ImGui::SetTooltip("%s", desc);
        }
    }
    ImGui::EndDisabled();
}

static void draw_toolbar() {
    const bool running = runner_is_running();
    u32 selected = 0;
    for (u32 i = 0; i < g_bench_count; ++i) selected += s_ui.selected[i] && suite_visible(g_benches[i].suite);

    ImGui::BeginDisabled(running || selected == 0);
    if (theme_button_primary("Run")) ui_start_run();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (theme_button("Cancel")) runner_cancel();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (theme_button("All"))
        for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = suite_visible(g_benches[i].suite);
    ImGui::SameLine();
    if (theme_button("None")) for (u32 i = 0; i < g_bench_count; ++i) s_ui.selected[i] = false;
    ImGui::EndDisabled();

    const bool have_results = g_run.total > 0 && !running;
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_results);
    if (theme_button("Save results...")) s_save.open_request = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme_button("Compare...")) ui_request_tool(TOOL_COMPARE);

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
        if (!suite_visible(s)) continue;
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
        if (theme_checkbox("##all", &all, sel > 0 && sel < total) && !running)     // dash when partly selected
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
                    // Group checkbox in the status column, like the suite's: dash when partly selected.
                    u32 gtotal = 0, gsel = 0;
                    for (u32 k = i; k < g_bench_count && g_benches[k].suite == s && !strcmp(g_benches[k].group, group); ++k) {
                        ++gtotal;
                        gsel += s_ui.selected[k];
                    }
                    ImGui::AlignTextToFramePadding();
                    group_open = ImGui::TreeNodeEx(group, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                              ImGuiTreeNodeFlags_AllowOverlap);
                    const f32 row_h = ImGui::GetItemRectSize().y;
                    ImGui::SameLine(s_status_x);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_h - theme_checkbox_height()) * 0.5f);
                    bool gall = gsel == gtotal;
                    ImGui::PushID(group);
                    if (theme_checkbox("##group", &gall, gsel > 0 && gsel < gtotal) && !running)
                        for (u32 k = i; k < i + gtotal; ++k) s_ui.selected[k] = gall;
                    ImGui::PopID();
                }
                if (group_open) draw_test_row(i, running);
            }
            if (group_open) ImGui::TreePop();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// ---- live panel -----------------------------------------------------------------

// Latest GPU 3D/2D frame (texture owned by main_gui.cpp), shown next to the live stats.
struct PreviewState {
    void* texture;
    u32 w, h;
};
static PreviewState s_preview;

void ui_set_preview(void* texture, u32 w, u32 h) { s_preview = { texture, w, h }; }

static void draw_live() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    if (!runner_is_running() || cur < 0) {
        if (g_run.total == 0) ImGui::TextDisabled("Select tests on the left and press Run.");
        else ImGui::TextDisabled("Run finished in %.1f s.", g_run.end_time - g_run.start_time);
        return;
    }
    const BenchDesc* b = &g_benches[cur];

    // With a preview: stats on the left, the rendered frame on the right (full panel height, aspect kept).
    const bool preview = s_preview.texture && s_preview.w && (b->flags & BENCH_PREVIEW);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    f32 img_w = 0.0f, img_h = 0.0f;
    if (preview) {
        img_h = avail.y;
        img_w = img_h * s_preview.w / s_preview.h;
        if (img_w > avail.x * 0.6f) {
            img_w = avail.x * 0.6f;
            img_h = img_w * s_preview.h / s_preview.w;
        }
        ImGui::BeginChild("##stats", ImVec2(avail.x - img_w - ImGui::GetStyle().ItemSpacing.x * 2.0f, avail.y),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    }

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

    if (preview) {
        ImGui::EndChild();
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.y += (avail.y - img_h) * 0.5f;
        ImGui::GetWindowDrawList()->AddImageRounded((ImTextureID)(intptr_t)s_preview.texture, p,
                                                    ImVec2(p.x + img_w, p.y + img_h), ImVec2(0, 0), ImVec2(1, 1),
                                                    IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
        ImGui::Dummy(ImVec2(img_w, avail.y));
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

static void draw_results() {
    // Material data table: horizontal dividers only, quiet header.
    const int cols = 4;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("results", cols, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 2.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    // Header row by hand so the number columns can be right-aligned over their numbers.
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < cols; ++c) {
        ImGui::TableSetColumnIndex(c);
        const char* name = ImGui::TableGetColumnName(c);
        if (c == 2) align_right(ImGui::CalcTextSize(name).x);
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
            char fps[32];
            theme_push_numbers();
            if (format_frame_fps(fps, sizeof(fps), m[k].name, m[k].unit, m[k].value)) {
                // "0.134 (7490 fps)": the fps part dimmed, both right-aligned as one block.
                align_right(ImGui::CalcTextSize(v).x + ImGui::CalcTextSize(fps).x);
                ImGui::TextUnformatted(v);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextDisabled("%s", fps);
            } else {
                text_right(v);
            }
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", m[k].unit);
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
    draw_targets();
    draw_toolbar();
    draw_save_dialog();
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
