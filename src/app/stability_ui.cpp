// Stability window: test selection, duration, sizes and safety limits on the left; live verdict, per-test
// status, errors per logical processor, sensor graphs, WHEA count and the first errors on the right. The report
// is saved to the results folder when a run ends (stability_<date>_<time>_<cpu>.toon.txt).
#include "app/stability_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "imgui.h"
#include "stability/stability.h"
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct StabUi {
    StabConfig cfg;
    bool initialized;
    bool dev;                   // --dev: offer the fault-injection self-test
    bool was_running;
    char message[400];          // last start error / saved report path
    bool message_bad;
    i32 duration_idx;
    StabSensors sens;
    StabError log[STAB_MAX_LOG];
};
static StabUi s_ui;

static const u32 DURATIONS[] = { 20, 60, 300, 600, 900, 1800, 3600, 0 };
static const char* const DURATION_NAMES[] = { "20 seconds", "1 minute", "5 minutes", "10 minutes", "15 minutes",
                                              "30 minutes", "1 hour", "Until stopped" };

void stability_ui_on_open() {
    if (s_ui.initialized) return;
    s_ui.initialized = true;
    s_ui.cfg.run[STAB_CPU] = s_ui.cfg.run[STAB_MEM] = true;
    s_ui.duration_idx = 3;      // 10 minutes
    s_ui.dev = wcsstr(GetCommandLineW(), L"--dev") != nullptr;
    // Dev / screenshots: --stability-run <ids> [seconds] starts a run right away (default 20 s; 1 GiB memory /
    // VRAM, 64 MiB disk file, 2 s per core: the verification limits). With --dev, --inject runs the self-test.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--stability-run") != 0) continue;
        char ids[128];
        WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, ids, sizeof(ids), nullptr, nullptr);
        StabConfig c = s_ui.cfg;
        c.seconds = i + 2 < argc && argv[i + 2][0] != L'-' ? (u32)_wtoi(argv[i + 2]) : 20;
        c.gpu_adapter = ui_params()->gpu_adapter;
        c.mem_bytes = c.vram_bytes = 1 * GiB;
        c.disk_bytes = 64 * MiB;
        c.core_seconds = 2;
        c.inject = s_ui.dev && wcsstr(GetCommandLineW(), L"--inject") != nullptr;
        s_ui.cfg.inject = c.inject;
        char err[160];
        if (!stab_parse_ids(ids, &c, err, sizeof(err)) || !stab_start(&c, err, sizeof(err))) {
            snprintf(s_ui.message, sizeof(s_ui.message), "%s", err);
            s_ui.message_bad = true;
        }
        s_ui.cfg.run[STAB_CPU] = s_ui.cfg.run[STAB_MEM] = false;
        for (u32 t = 0; t < STAB_COUNT; ++t) s_ui.cfg.run[t] = c.run[t];
        s_ui.cfg.together = c.together;
    }
    if (argv) LocalFree(argv);
}

static void save_report() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ui_results_dir(dir);
    const time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    char cpu[48];
    cpu_short_name(g_sys.cpu.brand, cpu, sizeof(cpu));
    wchar_t wcpu[48];
    MultiByteToWideChar(CP_UTF8, 0, cpu, -1, wcpu, 48);
    swprintf(path, MAX_PATH, L"%s\\stability_%04d%02d%02d_%02d%02d%02d_%s.toon.txt", dir, t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, wcpu);
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, sizeof(path8), nullptr, nullptr);
    const bool ok = stab_write_toon(path);
    snprintf(s_ui.message, sizeof(s_ui.message), ok ? "Report saved: %s" : "Failed to write %s", path8);
    s_ui.message_bad = !ok;
}

static void section(const char* t) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

// Row label in a fixed column, vertically centered on the widget that follows.
static void row_label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 6.5f);
}

static void limit_slider(const char* label, const char* id, f32* v, f32 lo, f32 hi, const char* help) {
    row_label(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat(id, v, 0.0f, hi, *v < lo ? "off" : "%.0f \xC2\xB0" "C", ImGuiSliderFlags_AlwaysClamp);
    if (*v > 0.0f && *v < lo) *v = 0.0f;   // below the useful range = off
    theme_help(help);
}

// ---- settings (left) -----------------------------------------------------------------------------------

static void draw_settings(bool running) {
    StabConfig* c = &s_ui.cfg;
    ImGui::BeginDisabled(running);
    section("Tests");
    if (ImGui::BeginTable("##tests_pick", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (u32 t = 0; t < STAB_COUNT; ++t) {
            ImGui::TableNextColumn();
            ImGui::PushID((int)t);
            if (t == STAB_POWER) ImGui::PushStyleColor(ImGuiCol_Text, g_pal.warn);
            theme_checkbox(g_stab_tests[t].name, &c->run[t]);
            if (t == STAB_POWER) ImGui::PopStyleColor();
            theme_help(g_stab_tests[t].help);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    section("Run");
    if (ImGui::RadioButton("One after another", !c->together)) c->together = false;
    theme_help("Each ticked test runs for the chosen time, in the order listed.");
    ImGui::SameLine();
    if (ImGui::RadioButton("All at once", c->together)) c->together = true;
    theme_help("All ticked tests run at the same time for the chosen time: combined CPU + GPU (+ memory) load that "
               "stresses the power supply, VRMs and cooling together.");
    row_label("Duration");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##duration", &s_ui.duration_idx, DURATION_NAMES, (int)ARRAY_COUNT(DURATION_NAMES));
    theme_help("Per test when they run one after another, in total when they run at once.");

    section("Options");
    int k = c->cpu_kernel;
    row_label("CPU kernel");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##kernel", &k, g_cpu_kernel_names, CK_COUNT);
    c->cpu_kernel = (CpuKernel)k;
    theme_help("Kernel for the CPU and per-core tests. Mixed rotates integer, SSE, AVX2, AVX-512 (if present) and the "
               "large-data kernel every 4 seconds; small-data FMA kernels make the most heat, the integer and SSE "
               "ones run at higher clocks.");
    int core_s = (int)c->core_seconds;
    row_label("Per core");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##coresec", &core_s, 5, 600, "%d s on each core", ImGuiSliderFlags_AlwaysClamp);
    c->core_seconds = (u32)core_s;
    theme_help("Per-core cycling: time on each physical core before moving to the next.");
    int mem = (int)c->mem_pct;
    row_label("Memory");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##mempct", &mem, 10, 90, "%d%% of available RAM", ImGuiSliderFlags_AlwaysClamp);
    c->mem_pct = (u32)mem;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    char tip[256];
    snprintf(tip, sizeof(tip), "Memory test size as a share of the RAM available now (%.1f GiB). At least 2 GiB always "
             "stays free for Windows. The buffer is locked so it stays in RAM.", ms.ullAvailPhys / (f64)GiB);
    theme_help(tip);
    int vram = (int)c->vram_pct;
    row_label("VRAM");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##vrampct", &vram, 10, 95, "%d%% of free VRAM", ImGuiSliderFlags_AlwaysClamp);
    c->vram_pct = (u32)vram;
    theme_help("VRAM test size as a share of the GPU's free local memory budget (what Windows lets one program use).");
    const BenchParams* bp = ui_params();
    row_label("GPU");
    ImGui::TextDisabled("%s", bp->gpu_adapter < g_sys.gpu_count ? g_sys.gpus[bp->gpu_adapter].name : "none");
    theme_help("The GPU tests use the GPU selected in the main window.");
    row_label("Disk");
    char drive_label[8];
    snprintf(drive_label, sizeof(drive_label), "%c:", c->disk_drive);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
    if (ImGui::BeginCombo("##drive", drive_label)) {
        for (u32 i = 0; i < g_sys.drive_count; ++i) {
            char l[8];
            snprintf(l, sizeof(l), "%c:", g_sys.drives[i].letter);
            if (ImGui::Selectable(l, g_sys.drives[i].letter == c->disk_drive)) c->disk_drive = g_sys.drives[i].letter;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    static const u32 SIZES_MIB[] = { 64, 256, 1024, 4096 };
    static const char* const SIZE_NAMES[] = { "64 MiB file", "256 MiB file", "1 GiB file", "4 GiB file" };
    int si = 1;
    for (u32 i = 0; i < ARRAY_COUNT(SIZES_MIB); ++i)
        if (c->disk_bytes == SIZES_MIB[i] * MiB) si = (int)i;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##disksize", &si, SIZE_NAMES, (int)ARRAY_COUNT(SIZE_NAMES))) c->disk_bytes = SIZES_MIB[si] * MiB;
    theme_help("Disk verify: drive and size of the temporary test file (deleted afterwards).");

    section("Safety");
    limit_slider("CPU limit", "##cpulim", &c->max_cpu_c, 50.0f, 105.0f,
                 "Stop when the hottest ACPI thermal zone reaches this. Without a kernel driver that is the only CPU "
                 "temperature Windows exposes, and on many laptops it lags the CPU or doesn't follow it at all: watch "
                 "the vendor's tools as well.");
    limit_slider("GPU limit", "##gpulim", &c->max_gpu_c, 50.0f, 100.0f, "Stop when the selected GPU reaches this temperature.");
    limit_slider("Drive limit", "##drivelim", &c->max_drive_c, 40.0f, 85.0f, "Stop when any drive reaches this temperature.");
    theme_checkbox("Stop at the first error", &c->stop_on_error);
    theme_help("Otherwise the run continues and counts every error (useful to see whether errors cluster on one core "
               "or memory region).");
    if (s_ui.dev) {
        theme_checkbox("Self-test: inject one fault per test", &c->inject);
        theme_help("Dev: each test corrupts one value on purpose so you can see that detection, attribution and the "
                   "report work. A run with this ticked always fails.");
    }
    ImGui::EndDisabled();
}

// ---- live (right) --------------------------------------------------------------------------------------

static ImVec4 status_color(StabStatus s) {
    switch (s) {
        case SS_RUNNING: return g_pal.primary;
        case SS_PASS: return g_pal.ok;
        case SS_FAIL: return g_pal.error;
        case SS_STOPPED: case SS_ERROR: return g_pal.warn;
        default: return g_pal.on_surface_variant;
    }
}

static void draw_verdict(bool running) {
    char why[256] = "";
    const char* verdict = running ? "running" : stab_verdict(why, sizeof(why));
    const u64 errors = stab_total_errors();
    ImVec4 col = g_pal.primary;
    const char* title = "Running";
    if (!running) {
        title = !strcmp(verdict, "pass") ? "Pass" : !strcmp(verdict, "fail") ? "Fail" : !strcmp(verdict, "stopped") ? "Stopped" : "Incomplete";
        col = !strcmp(verdict, "pass") ? g_pal.ok : !strcmp(verdict, "fail") ? g_pal.error : g_pal.warn;
    } else if (errors) {
        title = "Running - errors found";
        col = g_pal.error;
    }
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.5f));
    ImGui::TextColored(col, "%s", title);
    ImGui::PopFont();
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    const f64 el = stab_elapsed();
    const u32 total = g_stab.cfg.seconds;
    u32 tests = 0;
    for (u32 t = 0; t < STAB_COUNT; ++t) tests += g_stab.cfg.run[t];
    const u32 planned = total ? (g_stab.cfg.together ? total : total * tests) : 0;
    ImGui::AlignTextToFramePadding();
    theme_push_numbers();
    if (planned) ImGui::Text("%u:%02u of %u:%02u", (u32)el / 60, (u32)el % 60, planned / 60, planned % 60);
    else ImGui::Text("%u:%02u", (u32)el / 60, (u32)el % 60);
    ImGui::PopFont();
    if (!running && why[0]) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(why);
        ImGui::PopTextWrapPos();
    }
    if (planned && running) theme_progress((f32)(el / planned), ImGui::GetFontSize() * 0.25f);
}

static void draw_tests() {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##tests", 5, flags)) return;
    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("Errors", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Checked", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Now / notes", ImGuiTableColumnFlags_WidthStretch, 2.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    for (u32 t = 0; t < STAB_COUNT; ++t) {
        const StabTestState* s = &g_stab.tests[t];
        if (!g_stab.cfg.run[t]) continue;
        const StabStatus st = (StabStatus)s->status.load();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(g_stab_tests[t].name);
        ImGui::TableNextColumn();
        ImGui::TextColored(status_color(st), "%s", stab_status_name(st));
        ImGui::TableNextColumn();
        theme_push_numbers();
        const u64 e = s->errors.load();
        ImGui::TextColored(e ? g_pal.error : g_pal.on_surface, "%llu", e);
        const f64 fe = s->first_error_s.load();
        if (fe >= 0.0) theme_help("Time to the first error, counted from the start of this test.");
        if (fe >= 0.0) ImGui::TextDisabled("first %.1f s", fe);
        ImGui::TableNextColumn();
        const f64 w = s->work.load();
        if (w > 0.0) ImGui::Text("%.*f %s", w >= 100.0 ? 0 : 2, w, g_stab_tests[t].work_unit);
        ImGui::PopFont();
        ImGui::TableNextColumn();
        const char* phase = s->phase.load();
        if (phase && st == SS_RUNNING) ImGui::TextUnformatted(phase);
        if (s->note[0]) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", s->note);
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndTable();
}

// Grid of logical processors for the per-LP tests: green = tested without errors, red = errors (count inside).
static void draw_lp_grid() {
    bool any = false;
    for (u32 t = 0; t < STAB_COUNT; ++t)
        any |= (t == STAB_CPU || t == STAB_CACHE || t == STAB_CORES || t == STAB_MEM) && g_stab.cfg.run[t] && g_stab.tests[t].start_t > 0.0;
    if (!any || !g_topo.lp_count) return;
    section("Errors by logical processor");
    theme_help("CPU, cache, per-core and memory errors, by the logical processor that made or saw them. Hover a cell "
               "for its physical core. Errors that keep landing on one core point at that core (Curve Optimizer / "
               "undervolt); errors spread over all of them point at RAM, the memory controller or power delivery.");
    const f32 cell = ImGui::GetFontSize() * 2.6f, gap = 4.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const u32 n = g_topo.lp_count < STAB_MAX_LP ? g_topo.lp_count : STAB_MAX_LP;
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const u32 per_row = (u32)((avail + gap) / (cell + gap)) > 0 ? (u32)((avail + gap) / (cell + gap)) : 1;
    for (u32 lp = 0; lp < n; ++lp) {
        u32 errors = 0;
        f32 secs = 0.0f;
        for (u32 t = 0; t < STAB_COUNT; ++t) {
            errors += g_stab.tests[t].lp_errors[lp].load();
            secs += g_stab.tests[t].lp_seconds[lp].load();
        }
        if (lp % per_row) ImGui::SameLine(0.0f, gap);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID((int)lp);
        ImGui::InvisibleButton("##lp", ImVec2(cell, cell * 0.8f));
        ImGui::PopID();
        const ImVec4 base = errors ? g_pal.error : g_pal.ok;
        dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell * 0.8f), ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, 0.22f)), 4.0f);
        char txt[16];
        snprintf(txt, sizeof(txt), "%u", errors ? errors : lp);     // the LP number, or its error count in red
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2(p.x + (cell - ts.x) * 0.5f, p.y + (cell * 0.8f - ts.y) * 0.5f),
                    ImGui::GetColorU32(errors ? g_pal.error : g_pal.on_surface_variant), txt);
        if (ImGui::IsItemHovered()) {
            if (secs > 0.0f) ImGui::SetTooltip("LP %u, core %u: %u errors, %.0f s alone on this core", lp, g_topo.core[lp], errors, secs);
            else ImGui::SetTooltip("LP %u, core %u: %u errors", lp, g_topo.core[lp], errors);
        }
    }
}

// One sensor graph: name, current / max, line over the run's history (NaN gaps bridged).
static void graph(const char* name, StabSeries s, const char* fmt, f32 lo, f32 hi) {
    const StabSensors* d = &s_ui.sens;
    static f32 vals[STAB_HIST];
    u32 n = 0;
    f32 last = NAN;
    for (u32 i = 0; i < d->hist_count; ++i) {
        if (!isnan(d->hist[s][i])) last = d->hist[s][i];
        if (!isnan(last)) vals[n++] = last;
    }
    char head[96], now[32] = "\xE2\x80\x94", mx[32] = "";
    if (!isnan(d->now[s])) snprintf(now, sizeof(now), fmt, d->now[s]);
    if (!isnan(d->max[s])) {
        char m[24];
        snprintf(m, sizeof(m), fmt, d->max[s]);
        snprintf(mx, sizeof(mx), "  max %s", m);
    }
    snprintf(head, sizeof(head), "%s  %s%s", name, now, mx);
    ImGui::TextUnformatted(head);
    if (n > 1) {
        f32 a = lo, b = hi;
        if (isnan(a) || isnan(b)) {
            a = b = vals[0];
            for (u32 i = 0; i < n; ++i) { a = vals[i] < a ? vals[i] : a; b = vals[i] > b ? vals[i] : b; }
            a -= 2.0f;
            b += 2.0f;
        }
        ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
        ImGui::PushID((int)s);
        ImGui::PlotLines("##g", vals, (int)n, 0, nullptr, a, b, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.2f));
        ImGui::PopID();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::TextDisabled(n ? "Collecting\xE2\x80\xA6" : "Not reported");
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 1.2f));
    }
}

static void draw_graphs() {
    section("Sensors");
    theme_help("Sampled once a second while a test runs. CPU temperature is the hottest ACPI thermal zone (no kernel "
               "driver); it often lags the CPU on laptops. Drive temperature is the hottest drive.");
    if (!ImGui::BeginTable("##graphs", 2, ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableNextColumn(); graph("CPU clock", SH_CPU_MHZ, "%.0f MHz", NAN, NAN);
    ImGui::TableNextColumn(); graph("CPU load", SH_CPU_UTIL, "%.0f%%", 0.0f, 100.0f);
    ImGui::TableNextColumn(); graph("CPU temperature (ACPI)", SH_CPU_TEMP, "%.1f \xC2\xB0" "C", NAN, NAN);
    ImGui::TableNextColumn(); graph("Drive temperature", SH_DRIVE_TEMP, "%.0f \xC2\xB0" "C", NAN, NAN);
    ImGui::TableNextColumn(); graph("GPU temperature", SH_GPU_TEMP, "%.0f \xC2\xB0" "C", NAN, NAN);
    ImGui::TableNextColumn(); graph("GPU load", SH_GPU_UTIL, "%.0f%%", 0.0f, 100.0f);
    ImGui::TableNextColumn(); graph("GPU clock", SH_GPU_MHZ, "%.0f MHz", NAN, NAN);
    ImGui::TableNextColumn(); graph("GPU power", SH_GPU_POWER, "%.0f%% of limit", 0.0f, 100.0f);
    ImGui::EndTable();
}

static void draw_errors() {
    const u32 n = stab_error_log(s_ui.log, STAB_MAX_LOG);
    if (!n) return;
    section("First errors");
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const f32 h = ImGui::GetTextLineHeightWithSpacing() * (n < 8 ? n + 1.5f : 9.5f);
    if (!ImGui::BeginTable("##errors", 7, flags, ImVec2(0.0f, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Expected", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Actual", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    for (u32 i = 0; i < n; ++i) {
        const StabError* e = &s_ui.log[i];
        ImGui::TableNextRow();
        theme_push_numbers();
        ImGui::TableNextColumn(); ImGui::Text("%.2f s", e->t);
        ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(g_stab_tests[e->test].name);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e->what);
        theme_push_numbers();
        ImGui::TableNextColumn();
        if (e->lp >= 0) ImGui::Text("LP %d (core %u)", e->lp, e->lp < (i32)MAX_LP ? g_topo.core[e->lp] : 0);
        else if (e->chunk >= 0) ImGui::Text("chunk %d +0x%llX", e->chunk, e->address);
        else ImGui::Text("0x%llX", e->address);
        if (e->lp >= 0 && e->test == STAB_MEM) ImGui::TextDisabled("0x%llX", e->address);
        ImGui::TableNextColumn(); ImGui::Text("%016llX", e->expected);
        ImGui::TableNextColumn(); ImGui::Text("%016llX", e->actual);
        ImGui::TableNextColumn(); ImGui::TextColored(g_pal.error, "%016llX", e->expected ^ e->actual);
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void stability_ui_draw() {
    stability_ui_on_open();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##stability", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const bool running = stab_is_running();
    if (s_ui.was_running && !running) {     // a run just ended
        stab_wait();
        save_report();
    }
    s_ui.was_running = running;
    const bool started = g_stab.start_t > 0.0;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Looks for errors instead of measuring speed: every test checks what it computes or stores. Any "
                        "error, or any WHEA hardware error Windows logs meanwhile, means the system is not stable at its "
                        "current clocks, voltages or temperatures.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("settings", ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableSetupColumn("live", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableNextColumn();
        ImGui::BeginChild("##left", ImVec2(0.0f, 0.0f));
        ImGui::Spacing();
        const bool bench = runner_is_running();
        if (!running) {
            ImGui::BeginDisabled(bench);
            if (theme_button_primary("Start")) {
                StabConfig c = s_ui.cfg;
                c.seconds = DURATIONS[s_ui.duration_idx];
                c.gpu_adapter = ui_params()->gpu_adapter;
                char err[160];
                s_ui.message[0] = 0;
                if (!stab_start(&c, err, sizeof(err))) {
                    snprintf(s_ui.message, sizeof(s_ui.message), "%s", err);
                    s_ui.message_bad = true;
                }
            }
            ImGui::EndDisabled();
            if (bench) theme_help("A benchmark run is in progress; start the stability test when it has finished.");
        } else if (theme_button("Stop")) {
            stab_stop("stopped by user");
        }
        ImGui::SameLine();
        if (theme_button("Results folder")) {
            wchar_t dir[MAX_PATH];
            ui_results_dir(dir);
            ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (s_ui.message[0]) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(s_ui.message_bad ? g_pal.error : g_pal.on_surface_variant, "%s", s_ui.message);
            ImGui::PopTextWrapPos();
        }
        draw_settings(running);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Coverage note: the memory test runs inside Windows and reaches only the RAM Windows gives "
                            "it; a bootable tester such as MemTest86 covers nearly all of it.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("##right", ImVec2(0.0f, 0.0f));
        if (!started) {
            ImGui::Spacing();
            ImGui::TextDisabled("Tick the tests on the left and press Start.");
        } else {
            draw_verdict(running);
            stab_sensors(&s_ui.sens);
            ImGui::Spacing();
            draw_tests();
            ImGui::Spacing();
            theme_push_numbers();
            const u32 whea = g_stab.whea_corrected + g_stab.whea_uncorrected;
            ImGui::TextColored(whea ? g_pal.error : g_pal.on_surface, "WHEA hardware errors: %u corrected, %u uncorrected",
                               g_stab.whea_corrected, g_stab.whea_uncorrected);
            ImGui::PopFont();
            theme_help(g_stab.whea_active
                           ? "Windows Hardware Error Architecture events (Microsoft-Windows-WHEA-Logger in the System log) "
                             "logged during this run: corrected machine-check, cache, PCIe or memory errors. Any of them "
                             "under load is a sign of instability even when every test result was correct."
                           : "Not monitored: the System event log couldn't be subscribed.");
            draw_lp_grid();
            draw_graphs();
            draw_errors();
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}
