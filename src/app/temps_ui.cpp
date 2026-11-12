// Sensors window, three tabs:
//   Overview          Task Manager style: a device list on the left (CPU, each GPU, memory, drives, power,
//                     temperatures, PawnIO; each with a sparkline and a one-line summary) and the selected device's
//                     graphs on the right: a large utilization graph, big stat numbers, tiles per logical processor,
//                     per-core power, GPU clocks / VRAM / power / temperature, battery, thermal zones. "All" stacks
//                     every section. Graphs span 1 to 10 minutes (graph.h draws them).
//   Sensors           every channel (core/sensors.h) with now / min / max / avg (resettable) and a graph;
//                     pick channels for the log and the "Picked" view, tick channels for the overlay.
//   Log and overlay   sensor log to CSV at a chosen interval; the on-screen overlay.
// A search box filters the first two tabs (not saved; on the Overview it shows every matching section). Picks,
// overlay channels, log / overlay options, the selected device and the graph span persist in settings.ini [sensors].
#include "app/temps_ui.h"
#include "app/adv_ui.h"
#include "app/graph.h"
#include "app/overlay.h"
#include "app/settings.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/sensors.h"
#include "core/sysinfo.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SensorReading s_read[SENSOR_MAX];

// ---- preferences ([sensors] in settings.ini) --------------------------------------------------------------

static const u32 INTERVALS_MS[] = { 250, 500, 1000, 2000, 5000, 10000, 30000, 60000 };
static const char* const INTERVAL_NAMES[] = { "250 ms", "500 ms", "1 s", "2 s", "5 s", "10 s", "30 s", "60 s" };

struct Prefs {
    char picked[4096];          // comma-separated channel ids
    char osd[2048];             // overlay channel ids, in display order
    u32 interval;               // index into INTERVALS_MS
    OverlayCorner corner;
    u32 tab;
    bool picked_only, detail;
    u32 window;                 // Overview graph span, index into WINDOWS_S
    char device[16];            // Overview device ("all", "cpu", "gpu0", "memory", ...)
};
static Prefs s_prefs = { "", "", 2, OVL_TOP_RIGHT, 0, false, false, 0, "all" };
static bool s_prefs_loaded;

static void pref_read_str(const wchar_t* ini, const wchar_t* key, char* out, u32 size) {
    wchar_t w[4096];
    GetPrivateProfileStringW(L"sensors", key, L"", w, ARRAY_COUNT(w), ini);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, nullptr, nullptr);
}

static u32 pref_read_u32(const wchar_t* ini, const wchar_t* key, u32 def, u32 max) {
    const UINT v = GetPrivateProfileIntW(L"sensors", key, (INT)def, ini);
    return v <= max ? v : def;
}

static char s_view[16];         // --sensor-view (session only), applied after the ini

void temps_ui_set_view(const wchar_t* view) {
    WideCharToMultiByte(CP_UTF8, 0, view, -1, s_view, sizeof(s_view), nullptr, nullptr);
}

static void apply_view() {
    static const char* const tabs[] = { "overview", "sensors", "log" };
    if (!s_view[0]) return;
    s_prefs.tab = 0;
    bool tab = false;
    for (u32 i = 0; i < ARRAY_COUNT(tabs); ++i)
        if (!strcmp(s_view, tabs[i])) { s_prefs.tab = i; tab = true; }
    if (!tab) snprintf(s_prefs.device, sizeof(s_prefs.device), "%s", s_view);
    s_view[0] = 0;
}

static void prefs_load() {
    if (s_prefs_loaded) { apply_view(); return; }
    s_prefs_loaded = true;
    const wchar_t* ini = settings_ini_file();
    if (!ini) { apply_view(); return; }
    pref_read_str(ini, L"picked", s_prefs.picked, sizeof(s_prefs.picked));
    pref_read_str(ini, L"overlay", s_prefs.osd, sizeof(s_prefs.osd));
    s_prefs.interval = pref_read_u32(ini, L"log_interval", 2, ARRAY_COUNT(INTERVALS_MS) - 1);
    s_prefs.corner = (OverlayCorner)pref_read_u32(ini, L"overlay_corner", OVL_TOP_RIGHT, OVL_CORNER_COUNT - 1);
    s_prefs.tab = pref_read_u32(ini, L"tab", 0, 2);
    s_prefs.picked_only = pref_read_u32(ini, L"picked_only", 0, 1) != 0;
    s_prefs.detail = pref_read_u32(ini, L"per_core", 0, 1) != 0;
    s_prefs.window = pref_read_u32(ini, L"graph_window", 0, 3);
    pref_read_str(ini, L"device", s_prefs.device, sizeof(s_prefs.device));
    if (!s_prefs.device[0]) snprintf(s_prefs.device, sizeof(s_prefs.device), "all");
    apply_view();
}

static void prefs_save() {
    const wchar_t* ini = settings_ini_file();
    if (!ini) return;
    wchar_t w[4096];
    MultiByteToWideChar(CP_UTF8, 0, s_prefs.picked, -1, w, ARRAY_COUNT(w));
    WritePrivateProfileStringW(L"sensors", L"picked", w, ini);
    MultiByteToWideChar(CP_UTF8, 0, s_prefs.osd, -1, w, ARRAY_COUNT(w));
    WritePrivateProfileStringW(L"sensors", L"overlay", w, ini);
    WritePrivateProfileStringW(L"sensors", L"log_format", nullptr, ini);    // gone (the log is always CSV)
    MultiByteToWideChar(CP_UTF8, 0, s_prefs.device, -1, w, ARRAY_COUNT(w));
    WritePrivateProfileStringW(L"sensors", L"device", w, ini);
    const struct { const wchar_t* key; u32 v; } nums[] = {
        { L"log_interval", s_prefs.interval }, { L"overlay_corner", s_prefs.corner },
        { L"tab", s_prefs.tab }, { L"picked_only", s_prefs.picked_only }, { L"per_core", s_prefs.detail },
        { L"graph_window", s_prefs.window },
    };
    for (u32 i = 0; i < ARRAY_COUNT(nums); ++i) {
        swprintf(w, ARRAY_COUNT(w), L"%u", nums[i].v);
        WritePrivateProfileStringW(L"sensors", nums[i].key, w, ini);
    }
}

// Comma-separated id lists.
static bool list_has(const char* list, const char* id) {
    const size_t n = strlen(id);
    for (const char* p = strstr(list, id); p; p = strstr(p + 1, id))
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

static void list_set(char* list, u32 size, const char* id, bool on) {
    if (on == list_has(list, id)) return;
    if (on) {
        const size_t len = strlen(list);
        if (len + strlen(id) + 2 < size) snprintf(list + len, size - len, "%s%s", len ? "," : "", id);
        return;
    }
    const size_t n = strlen(id);
    for (char* p = strstr(list, id); p; p = strstr(p + 1, id)) {
        if ((p != list && p[-1] != ',') || (p[n] != 0 && p[n] != ',')) continue;
        char* from = p + n + (p[n] == ',' ? 1 : 0);
        char* to = p != list && p[n] == 0 ? p - 1 : p;      // last item: drop the comma before it
        memmove(to, from, strlen(from) + 1);
        return;
    }
}

static void apply_overlay(bool visible) { overlay_set(visible, s_prefs.osd, s_prefs.corner); }

void temps_ui_show_overlay() {
    prefs_load();
    apply_overlay(true);
}

// ---- shared bits ------------------------------------------------------------------------------------------

static CpuLive s_cpu;
static GpuLive s_gpus[8];
static PowerLive s_power;
static SensorChannel s_ch[CH_MAX];
static u32 s_ch_count;

// ---- search filter (Overview and Sensors tabs; not saved) ---------------------------------------------------

static char s_filter[128];

void temps_ui_set_filter(const wchar_t* text) {
    WideCharToMultiByte(CP_UTF8, 0, text, -1, s_filter, sizeof(s_filter), nullptr, nullptr);
}

static bool filtering() {
    for (const char* p = s_filter; *p; ++p)
        if (*p != ' ') return true;
    return false;
}

static bool has_text(const char* s, const char* word, size_t n) {     // case-insensitive substring
    for (; *s; ++s)
        if (!_strnicmp(s, word, n)) return true;
    return false;
}

// True when every space-separated word of the filter is in one of `texts` (empty filter: always true).
static bool filter_hit(const char* const* texts, u32 count) {
    for (const char* p = s_filter; *p;) {
        while (*p == ' ') ++p;
        const char* e = p;
        while (*e && *e != ' ') ++e;
        bool found = e == p;
        for (u32 i = 0; i < count && !found; ++i) found = texts[i] && has_text(texts[i], p, (size_t)(e - p));
        if (!found) return false;
        p = e;
    }
    return true;
}

// A channel matches on its name, id or group name ("CPU", "Graphics", "Temperatures", "Power").
static bool channel_hit(const SensorChannel* c) {
    const char* t[] = { c->name, c->id, g_channel_group_names[c->group] };
    return filter_hit(t, ARRAY_COUNT(t));
}

static void draw_filter() {
    const f32 fs = ImGui::GetFontSize();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(fs * 13.0f);
    ImGui::InputTextWithHint("##filter", "Search sensors", s_filter, sizeof(s_filter), ImGuiInputTextFlags_EscapeClearsAll);
    theme_help("Shows only the sensors whose name or id contains every word typed (Ctrl+F; Esc clears).");
    if (!s_filter[0]) return;
    ImGui::SameLine(0.0f, 4.0f);
    if (theme_tab("\xC3\x97##clear", false)) s_filter[0] = 0;
    theme_help("Clear the search.");
}

static void dash() { ImGui::TextColored(g_pal.outline, "\xE2\x80\x94"); }

// ---- Overview layout pieces -------------------------------------------------------------------------------

static const u32 WINDOWS_S[] = { 60, 120, 300, 600 };     // graph time spans (one sample per second)
static const char* const WINDOW_TABS[] = { "1 min", "2 min", "5 min", "10 min" };
static const char* const WINDOW_TEXT[] = { "60 seconds", "2 minutes", "5 minutes", "10 minutes" };

static u32 win() { return WINDOWS_S[s_prefs.window]; }

static f32 s_h[2][CH_HISTORY];      // scratch histories (each graph is drawn before the next fill)

static i32 channel_index(const char* id) {
    for (u32 i = 0; i < s_ch_count; ++i)
        if (!strcmp(s_ch[i].id, id)) return (i32)i;
    return -1;
}

static const SensorChannel* channel_by_id(const char* id) {
    const i32 i = channel_index(id);
    return i >= 0 ? &s_ch[i] : nullptr;
}

static bool id_hit(const char* id) {
    if (!filtering()) return true;
    const SensorChannel* c = channel_by_id(id);
    return c && channel_hit(c);
}

// A channel that has reported something since the window opened (or the last reset).
static bool has_data(const SensorChannel* c) { return c && (c->valid || c->count > 0); }

static u32 history(i32 ch, f32* out) { return ch >= 0 ? sensors_channel_history((u32)ch, out, win()) : 0; }

// Section header: big title left, `sub` (model name) right-aligned like Task Manager.
static void section(const char* title, const char* sub) {
    ImGui::Spacing();
    const f32 y0 = ImGui::GetCursorPosY();
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.45f));
    ImGui::TextUnformatted(title);
    const f32 big_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    if (sub && sub[0]) {
        ImGui::PushFont(g_fonts.medium, theme_font_size(1.05f));
        const f32 w = ImGui::CalcTextSize(sub).x;
        ImGui::SameLine();
        const f32 avail = ImGui::GetContentRegionAvail().x;
        if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
        ImGui::SetCursorPosY(y0 + (big_h - ImGui::GetTextLineHeight()) * 0.75f);
        ImGui::TextUnformatted(sub);
        ImGui::PopFont();
    }
    ImGui::Spacing();
}

static void subheading(const char* t) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

// Stat row: big numbers with small labels, wrapping when the line is full.
static void stat(bool* first, const char* label, const char* value, i32 kind = -1) {
    const f32 fs = ImGui::GetFontSize();
    if (!*first) {
        ImGui::SameLine(0.0f, fs * 2.2f);
        if (ImGui::GetContentRegionAvail().x < fs * 7.5f) ImGui::NewLine();
    }
    *first = false;
    graph_stat(label, value, kind);
}

// Time-series graph of channel `ch` (title left, current value right).
static void channel_graph(i32 ch, const char* title, GraphKind kind, f32 lo, f32 hi, f32 height, u32 flags = 0) {
    if (ch < 0) return;
    const SensorChannel* c = &s_ch[ch];
    const u32 n = history(ch, s_h[0]);
    char right[48];
    const i32 dec = !strcmp(c->unit, "%") ? 0 : c->decimals > 1 ? 1 : c->decimals;
    graph_format(right, sizeof(right), c->valid ? c->value : NAN, c->unit, dec);
    Graph g;
    g.title = title;
    g.right = right;
    g.unit = c->unit;
    g.kind = kind;
    g.flags = flags;
    g.lo = lo;
    g.hi = hi;
    g.min_span = kind == GK_TEMP ? 10.0f : 0.0f;
    g.decimals = c->decimals > 2 ? 2 : c->decimals;
    g.window = win();
    g.step_s = 1.0f;
    GraphLine line;
    line.y = s_h[0];
    line.n = n;
    line.name = title;
    graph_time(c->id, &g, &line, 1, ImVec2(0.0f, height));
}

// Grid of tiles: a table of `cols` equal columns with some room between the tiles.
static bool grid_begin(const char* id, u32 cols) {
    const f32 fs = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(fs * 0.35f, fs * 0.3f));
    const bool open = ImGui::BeginTable(id, (int)(cols ? cols : 1), ImGuiTableFlags_SizingStretchSame);
    if (!open) ImGui::PopStyleVar();
    return open;
}

static void grid_end() {
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

// Columns that fit tiles at least `min_w` font sizes wide, at most `max`.
static u32 grid_cols(f32 min_w, u32 max) {
    const u32 c = (u32)(ImGui::GetContentRegionAvail().x / (ImGui::GetFontSize() * min_w));
    return c < 1 ? 1 : c > max ? max : c;
}

// ---- Overview: CPU ----------------------------------------------------------------------------------------

static const char* const RAILS_HELP = "Energy Metering Interface: power rails the platform exposes to Windows (here "
                                      "RAPL-style energy counters). Averaged over the last second.";
static const char* const CLOCK_HELP = "Effective clock per logical processor (Windows' APERF/MPERF-based \"% Processor "
                                      "Performance\" x the nominal clock) and its load over the last second. Idle cores "
                                      "report parked clocks; \"busy cores\" averages only the ones at least half busy. "
                                      "Under full load these readings can lag by a second.";

static bool core_rail(u32 i) { return !strncmp(s_power.rail_name[i], "CPU core ", 9); }

static bool lp_hit(u32 i) {
    char a[16], b[16];
    snprintf(a, sizeof(a), "lp%u.mhz", i);
    snprintf(b, sizeof(b), "lp%u.load", i);
    return id_hit(a) || id_hit(b);
}

static bool cpu_summary_hit() { return id_hit("cpu.load") || id_hit("cpu.busy_mhz") || id_hit("cpu.max_mhz"); }

static bool cpu_match() {
    if (cpu_summary_hit()) return true;
    for (u32 i = 0; i < s_cpu.lps; ++i)
        if (lp_hit(i)) return true;
    for (u32 i = 0; i < s_power.rail_count; ++i)
        if (s_power.rail_cpu[i] && id_hit(s_power.rail_id[i])) return true;
    return false;
}

// The main CPU power rail (package), -1 if none.
static i32 cpu_main_rail() {
    i32 best = -1;
    for (u32 i = 0; i < s_power.rail_count; ++i) {
        if (!s_power.rail_cpu[i] || core_rail(i)) continue;
        if (!strcmp(s_power.rail_id[i], "emi.cpu_pkg")) return (i32)i;
        if (best < 0) best = (i32)i;
    }
    return best;
}

// CPU temperature for the stat row: a "cpu.temp*" channel (e.g. from PawnIO) or the hottest ACPI zone.
static f32 cpu_temp(const char** label, u32 n_read) {
    for (u32 i = 0; i < s_ch_count; ++i)
        if (!strncmp(s_ch[i].id, "cpu.temp", 8) && s_ch[i].valid) { *label = "Temperature"; return s_ch[i].value; }
    f32 t = NAN;
    for (u32 i = 0; i < n_read; ++i)
        if (s_read[i].group == SG_CPU && s_read[i].valid && !(s_read[i].temp_c <= t)) t = s_read[i].temp_c;
    *label = "Temperature (ACPI)";
    return t;
}

static void draw_cpu(bool full, u32 n_read) {
    const CpuLive* c = &s_cpu;
    section("CPU", g_sys.cpu.brand);
    if (!c->lps) {
        ImGui::TextDisabled("Reading\xE2\x80\xA6");
        return;
    }
    const f32 fs = ImGui::GetFontSize();
    const bool summary = cpu_summary_hit();
    const i32 pkg = cpu_main_rail();
    const bool pkg_shown = pkg >= 0 && id_hit(s_power.rail_id[pkg]);
    char v[48], title[96];
    if (summary) {
        snprintf(title, sizeof(title), "%% Utilization over %s", WINDOW_TEXT[s_prefs.window]);
        const u32 n = history(channel_index("cpu.load"), s_h[0]);
        Graph g;
        g.title = title;
        g.unit = "%";
        g.kind = GK_LOAD;
        g.lo = 0.0f;
        g.hi = 100.0f;
        g.window = win();
        g.step_s = 1.0f;
        GraphLine line;
        line.y = s_h[0];
        line.n = n;
        line.name = "Utilization";
        graph_time("##cpuload", &g, &line, 1, ImVec2(0.0f, fs * (full ? 11.0f : 7.5f)));

        ImGui::Spacing();
        bool first = true;
        snprintf(v, sizeof(v), "%.0f%%", c->avg_util);
        stat(&first, "Utilization", v, GK_LOAD);
        graph_format(v, sizeof(v), c->busy_mhz, "MHz");
        stat(&first, "Speed, busy cores", isnan(c->busy_mhz) ? "idle" : v, GK_FREQ);
        theme_help(CLOCK_HELP);
        const SensorChannel* fastest = channel_by_id("cpu.max_mhz");
        graph_format(v, sizeof(v), fastest && fastest->valid ? fastest->value : NAN, "MHz");
        stat(&first, "Fastest core", v, GK_FREQ);
        graph_format(v, sizeof(v), c->max_mhz, "MHz");
        stat(&first, "Highest seen", v);
        if (pkg >= 0) {
            graph_format(v, sizeof(v), s_power.rail_w[pkg], "W", 1);
            stat(&first, s_power.rail_name[pkg], v, GK_POWER);
            theme_help(RAILS_HELP);
        }
        const char* temp_label;
        const f32 t = cpu_temp(&temp_label, n_read);
        if (!isnan(t)) {
            graph_format(v, sizeof(v), t, "\xC2\xB0" "C", 1);
            stat(&first, temp_label, v, GK_TEMP);
        }
        snprintf(v, sizeof(v), "%u", c->lps);
        stat(&first, "Logical processors", v);
    }

    // Clock and package power over time, side by side.
    if (summary || pkg_shown) {
        ImGui::Spacing();
        const u32 cols = (summary ? 1 : 0) + (pkg_shown ? 1 : 0);
        if (grid_begin("##cpu_time", cols)) {
            if (summary) {
                ImGui::TableNextColumn();
                const u32 n0 = history(channel_index("cpu.busy_mhz"), s_h[0]);
                const u32 n1 = history(channel_index("cpu.max_mhz"), s_h[1]);
                snprintf(title, sizeof(title), "Clock over %s", WINDOW_TEXT[s_prefs.window]);
                Graph g;
                g.title = title;
                g.unit = "MHz";
                g.kind = GK_FREQ;
                g.flags = GF_ZERO;
                g.window = win();
                g.step_s = 1.0f;
                GraphLine lines[2];
                lines[0].y = s_h[0];
                lines[0].n = n0;
                lines[0].name = "Busy cores";
                lines[1].y = s_h[1];
                lines[1].n = n1;
                lines[1].name = "Fastest core";
                lines[1].kind = GK_LATENCY;
                graph_time("##clock", &g, lines, 2, ImVec2(0.0f, fs * 6.0f));
            }
            if (pkg_shown) {
                ImGui::TableNextColumn();
                snprintf(title, sizeof(title), "%s over %s", s_power.rail_name[pkg], WINDOW_TEXT[s_prefs.window]);
                channel_graph(channel_index(s_power.rail_id[pkg]), title, GK_POWER, NAN, NAN, fs * 6.0f, GF_ZERO);
            }
            grid_end();
        }
    }

    // Logical processors: load per LP, its clock in the caption.
    u32 lps = 0;
    for (u32 i = 0; i < c->lps; ++i) lps += lp_hit(i);
    if (lps) {
        subheading("Logical processors");
        theme_help(CLOCK_HELP);
        const u32 cols = c->lps <= 16 ? 4 : grid_cols(9.0f, 8);
        if (grid_begin("##lps", cols)) {
            char id[16], name[16], right[32];
            for (u32 i = 0; i < c->lps; ++i) {
                if (!lp_hit(i)) continue;
                ImGui::TableNextColumn();
                snprintf(id, sizeof(id), "lp%u.load", i);
                snprintf(name, sizeof(name), "LP %u", i);
                const u32 n = history(channel_index(id), s_h[0]);
                graph_format(right, sizeof(right), c->mhz[i], "MHz");
                Graph g;
                g.title = name;
                g.right = right;
                g.unit = "%";
                g.kind = GK_LOAD;
                g.lo = 0.0f;
                g.hi = 100.0f;
                g.window = win();
                g.step_s = 1.0f;
                g.decimals = 0;
                GraphLine line;
                line.y = s_h[0];
                line.n = n;
                line.name = name;
                graph_time(id, &g, &line, 1, ImVec2(0.0f, fs * (full ? 3.8f : 3.2f)));
            }
            grid_end();
        }
    }

    // CPU power rails other than the package: per core, PP0.
    u32 rails = 0;
    for (u32 i = 0; i < s_power.rail_count; ++i)
        rails += s_power.rail_cpu[i] && (i32)i != pkg && id_hit(s_power.rail_id[i]);
    if (rails) {
        subheading("Power per core");
        theme_help(RAILS_HELP);
        if (grid_begin("##rails", grid_cols(9.0f, 8))) {
            char name[64];
            for (u32 i = 0; i < s_power.rail_count; ++i) {
                if (!s_power.rail_cpu[i] || (i32)i == pkg || !id_hit(s_power.rail_id[i])) continue;
                ImGui::TableNextColumn();
                if (core_rail(i)) snprintf(name, sizeof(name), "Core %s", s_power.rail_name[i] + 9);
                else snprintf(name, sizeof(name), "%s", s_power.rail_name[i]);
                channel_graph(channel_index(s_power.rail_id[i]), name, GK_POWER, NAN, NAN, fs * 3.2f, GF_ZERO);
            }
            grid_end();
        }
    }
}

// ---- Overview: GPUs ---------------------------------------------------------------------------------------

static const char* pcie_rate(u8 gen) {
    switch (gen) {
        case 1: return "2.5 GT/s"; case 2: return "5 GT/s"; case 3: return "8 GT/s";
        case 4: return "16 GT/s"; case 5: return "32 GT/s"; case 6: return "64 GT/s"; default: return "?";
    }
}

static i32 gpu_channel(u32 gpu, const char* key) {
    char id[40];
    snprintf(id, sizeof(id), "gpu%u.%s", gpu, key);
    return channel_index(id);
}

static bool gpu_match(u32 i) {
    static const char* const keys[] = { "core_mhz", "mem_mhz", "load", "vram_mib", "power_pct", "fan_rpm", "temp" };
    if (!filtering()) return true;
    for (u32 k = 0; k < ARRAY_COUNT(keys); ++k) {
        const i32 ch = gpu_channel(i, keys[k]);
        if (ch >= 0 && channel_hit(&s_ch[ch])) return true;
    }
    return false;
}

static void draw_gpu(u32 i, bool full) {
    const GpuLive* gl = &s_gpus[i];
    const GpuReading* r = &gl->r;
    const f32 fs = ImGui::GetFontSize();
    char head[16], v[48], title[96];
    snprintf(head, sizeof(head), "GPU %u", i);
    ImGui::PushID((int)i);
    section(head, gl->name);
    if (!gl->opened) {
        ImGui::TextDisabled("No live readings for this adapter.");
        ImGui::PopID();
        return;
    }
    auto shown = [&](const char* key) {
        const i32 ch = gpu_channel(i, key);
        return ch >= 0 && has_data(&s_ch[ch]) && (!filtering() || channel_hit(&s_ch[ch]));
    };
    if (shown("load") || (!filtering() && gl->hist_count)) {
        snprintf(title, sizeof(title), "%% Utilization over %s", WINDOW_TEXT[s_prefs.window]);
        const u32 n = history(gpu_channel(i, "load"), s_h[0]);
        Graph g;
        g.title = title;
        g.unit = "%";
        g.kind = GK_LOAD;
        g.lo = 0.0f;
        g.hi = 100.0f;
        g.window = win();
        g.step_s = 1.0f;
        GraphLine line;
        line.y = s_h[0];
        line.n = n;
        line.name = "Utilization";
        graph_time("##gpuload", &g, &line, 1, ImVec2(0.0f, fs * (full ? 9.0f : 6.5f)));
    }

    ImGui::Spacing();
    bool first = true;
    graph_format(v, sizeof(v), r->util, "%");
    stat(&first, "Utilization", v, GK_LOAD);
    graph_format(v, sizeof(v), r->core_mhz, "MHz");
    stat(&first, "Core clock", v, GK_FREQ);
    if (!isnan(r->mem_mhz)) {
        graph_format(v, sizeof(v), r->mem_mhz, "MHz");
        stat(&first, "Memory clock", v, GK_LATENCY);
    }
    if (!isnan(r->vram_used_mib)) {
        char used[24], total[24];
        graph_format(used, sizeof(used), r->vram_used_mib, "MiB");
        graph_format(total, sizeof(total), gl->vram_total_mib, "MiB");
        if (gl->vram_total_mib > 0.0f) snprintf(v, sizeof(v), "%s / %s", used, total);
        else snprintf(v, sizeof(v), "%s", used);
        stat(&first, "Dedicated memory", v, GK_MEMORY);
    }
    if (!isnan(r->power_pct)) {
        snprintf(v, sizeof(v), "%.0f%%", r->power_pct);
        stat(&first, "Power (of limit)", v, GK_POWER);
    }
    if (!isnan(r->temp_c)) {
        graph_format(v, sizeof(v), r->temp_c, "\xC2\xB0" "C", 0);
        stat(&first, "Temperature", v, GK_TEMP);
    }
    if (!isnan(r->fan_rpm)) {
        snprintf(v, sizeof(v), "%.0f RPM", r->fan_rpm);
        stat(&first, "Fan", v, GK_OTHER);
    }
    if (r->link_gen && r->link_width) {
        snprintf(v, sizeof(v), "Gen %u x%u", r->link_gen, r->link_width);
        stat(&first, "PCIe link", v);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Link now: %s x%u. The GPU itself supports %s x%u (Gen %u x%u); the slot or the laptop's "
                              "wiring may allow less. GPUs drop to a slower link when idle to save power and speed it up "
                              "under load.", pcie_rate(r->link_gen), r->link_width, pcie_rate(r->link_gen_max),
                              r->link_width_max, r->link_gen_max, r->link_width_max);
    }

    // Tiles: clocks, memory, power, temperature, fan (the ones this GPU reports).
    struct Tile { const char* key; const char* title; GraphKind kind; f32 lo, hi; u32 flags; };
    const Tile tiles[] = {
        { "core_mhz", "Core clock", GK_FREQ, NAN, NAN, GF_ZERO },
        { "mem_mhz", "Memory clock", GK_LATENCY, NAN, NAN, GF_ZERO },
        { "vram_mib", "Dedicated memory", GK_MEMORY, 0.0f, gl->vram_total_mib > 0.0f ? gl->vram_total_mib : NAN, GF_ZERO },
        { "power_pct", "Power (of limit)", GK_POWER, 0.0f, 100.0f, 0 },
        { "temp", "Temperature", GK_TEMP, NAN, NAN, 0 },
        { "fan_rpm", "Fan", GK_OTHER, NAN, NAN, GF_ZERO },
    };
    u32 count = 0;
    for (u32 k = 0; k < ARRAY_COUNT(tiles); ++k) count += shown(tiles[k].key);
    if (count) {
        ImGui::Spacing();
        if (grid_begin("##gpu_tiles", grid_cols(13.0f, count < 4 ? count : 4))) {
            for (u32 k = 0; k < ARRAY_COUNT(tiles); ++k) {
                if (!shown(tiles[k].key)) continue;
                ImGui::TableNextColumn();
                channel_graph(gpu_channel(i, tiles[k].key), tiles[k].title, tiles[k].kind, tiles[k].lo, tiles[k].hi,
                              fs * 4.6f, tiles[k].flags);
            }
            grid_end();
        }
    }
    if (full || !filtering())
        ImGui::TextDisabled("Values the driver doesn't report show \xE2\x80\x94 (e.g. a laptop GPU that is asleep). Power is "
                            "the share of the GPU's power limit.");
    ImGui::PopID();
}

// ---- Overview: memory (GlobalMemoryStatusEx, sampled here once a second) ------------------------------------

static f32 s_mem_hist[CH_HISTORY];      // MiB in use, oldest first
static u32 s_mem_count;
static u64 s_mem_last_ms;
static MEMORYSTATUSEX s_mem;

static void mem_sample() {
    const u64 now = GetTickCount64();
    if (s_mem_count && now - s_mem_last_ms < 1000) return;
    s_mem_last_ms = now;
    s_mem.dwLength = sizeof(s_mem);
    if (!GlobalMemoryStatusEx(&s_mem)) return;
    const f32 used = (f32)((s_mem.ullTotalPhys - s_mem.ullAvailPhys) / MiB);
    if (s_mem_count == CH_HISTORY) memmove(s_mem_hist, s_mem_hist + 1, (CH_HISTORY - 1) * sizeof(f32));
    else ++s_mem_count;
    s_mem_hist[s_mem_count - 1] = used;
}

static bool mem_match() {
    static const char* const t[] = { "Memory", "RAM", "in use available committed" };
    return s_mem_count && filter_hit(t, ARRAY_COUNT(t));
}

static void draw_memory(bool full) {
    if (!s_mem_count) return;
    const f32 fs = ImGui::GetFontSize();
    char total[24], v[64], title[96];
    const f32 total_mib = (f32)(s_mem.ullTotalPhys / MiB), used = s_mem_hist[s_mem_count - 1];
    graph_format(total, sizeof(total), total_mib, "MiB");
    section("Memory", total);
    snprintf(title, sizeof(title), "Memory usage over %s", WINDOW_TEXT[s_prefs.window]);
    Graph g;
    g.title = title;
    g.right = total;
    g.unit = "MiB";
    g.kind = GK_MEMORY;
    g.lo = 0.0f;
    g.hi = total_mib;
    g.window = win();
    g.step_s = 1.0f;
    GraphLine line;
    line.y = s_mem_count > win() ? s_mem_hist + s_mem_count - win() : s_mem_hist;
    line.n = s_mem_count < win() ? s_mem_count : win();
    line.name = "In use";
    graph_time("##mem", &g, &line, 1, ImVec2(0.0f, fs * (full ? 9.0f : 6.0f)));
    ImGui::Spacing();
    bool first = true;
    graph_format(v, sizeof(v), used, "MiB");
    stat(&first, "In use", v, GK_MEMORY);
    graph_format(v, sizeof(v), (f32)(s_mem.ullAvailPhys / MiB), "MiB");
    stat(&first, "Available", v);
    char a[24], b[24];
    graph_format(a, sizeof(a), (f32)((s_mem.ullTotalPageFile - s_mem.ullAvailPageFile) / MiB), "MiB");
    graph_format(b, sizeof(b), (f32)(s_mem.ullTotalPageFile / MiB), "MiB");
    snprintf(v, sizeof(v), "%s / %s", a, b);
    stat(&first, "Committed", v);
    theme_help("Memory committed by all processes, of the commit limit (RAM plus the page file).");
    snprintf(v, sizeof(v), "%u%%", (u32)s_mem.dwMemoryLoad);
    stat(&first, "Load", v, GK_LOAD);
}

// ---- Overview: temperature tiles (thermal zones, drives) --------------------------------------------------

// The reading's channel ("temp.tz01", "disk1.temp": named "<reading>" or "<reading> temperature"), -1 if none.
static i32 reading_channel(const SensorReading* s) {
    char with[128];
    snprintf(with, sizeof(with), "%s temperature", s->name);
    for (u32 i = 0; i < s_ch_count; ++i)
        if (s_ch[i].group == CHG_TEMP && (!strcmp(s_ch[i].name, s->name) || !strcmp(s_ch[i].name, with))) return (i32)i;
    return -1;
}

static bool reading_hit(const SensorReading* s) {
    if (!filtering()) return true;
    const i32 ch = reading_channel(s);
    if (ch >= 0) return channel_hit(&s_ch[ch]);
    const char* t[] = { s->name, s->detail, g_channel_group_names[CHG_TEMP] };
    return filter_hit(t, ARRAY_COUNT(t));
}

static bool group_match(SensorGroup g, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (s_read[i].group == g && reading_hit(&s_read[i])) return true;
    return false;
}

static ImU32 temp_col(const SensorReading* s) {
    if (s->warn_c > 0.0f && s->temp_c >= s->warn_c) return ImGui::GetColorU32(g_pal.error);
    if (s->warn_c > 0.0f && s->temp_c >= s->warn_c - 10.0f) return ImGui::GetColorU32(g_pal.warn);
    return 0;
}

// One tile per reading of group `g`: the temperature over time, min / max and the warning level on hover.
static void draw_temp_tiles(SensorGroup g, u32 n, f32 height) {
    u32 count = 0;
    for (u32 i = 0; i < n; ++i) count += s_read[i].group == g && reading_hit(&s_read[i]);
    if (!count || !grid_begin(g == SG_DISK ? "##disk_temps" : "##zone_temps", grid_cols(15.0f, count < 4 ? count : 4)))
        return;
    char right[48], tip[400];
    for (u32 i = 0; i < n; ++i) {
        const SensorReading* s = &s_read[i];
        if (s->group != g || !reading_hit(s)) continue;
        ImGui::TableNextColumn();
        ImGui::PushID((int)i);
        const i32 ch = reading_channel(s);
        const u32 count = ch >= 0 ? history(ch, s_h[0]) : 0;
        graph_format(right, sizeof(right), s->valid ? s->temp_c : NAN, "\xC2\xB0" "C", 1);
        Graph gr;
        gr.title = s->name;
        gr.right = right;
        gr.right_col = s->valid ? temp_col(s) : 0;
        gr.unit = "\xC2\xB0" "C";
        gr.kind = GK_TEMP;
        gr.min_span = 10.0f;
        gr.decimals = 1;
        gr.window = win();
        gr.step_s = 1.0f;
        gr.flags = GF_NO_TOOLTIP;
        GraphLine line;
        line.y = ch >= 0 ? s_h[0] : s->history;
        line.n = ch >= 0 ? count : s->history_count;
        const GraphHit hit = graph_time("##t", &gr, &line, 1, ImVec2(0.0f, height));
        if (ImGui::IsItemHovered()) {
            u32 len = 0;
            if (hit.point >= 0) len += snprintf(tip + len, sizeof(tip) - len, "%.1f \xC2\xB0" "C\n", line.y[hit.point]);
            if (s->detail[0]) len += snprintf(tip + len, sizeof(tip) - len, "%s\n", s->detail);
            if (s->history_count)
                len += snprintf(tip + len, sizeof(tip) - len, "Min %.1f, max %.1f \xC2\xB0" "C since the window opened\n",
                                s->min_c, s->max_c);
            if (s->valid) snprintf(tip + len, sizeof(tip) - len, "Warning threshold: %.0f \xC2\xB0" "C", s->warn_c);
            else snprintf(tip + len, sizeof(tip) - len, "%s", s->note[0] ? s->note : "No reading yet.");
            ImGui::SetTooltip("%s", tip);
        }
        ImGui::PopID();
    }
    grid_end();
}

// ---- Overview: battery and the other power rails --------------------------------------------------------

static bool battery_hit(u32 b) {
    static const char* const keys[] = { "rate_w", "voltage_v", "charge_pct", "remaining_wh", "temp" };
    char id[40];
    for (u32 k = 0; k < ARRAY_COUNT(keys); ++k) {
        if (s_power.battery_count > 1) snprintf(id, sizeof(id), "bat%u.%s", b, keys[k]);
        else snprintf(id, sizeof(id), "bat.%s", keys[k]);
        if (id_hit(id)) return true;
    }
    return false;
}

static i32 battery_channel(u32 b, const char* key) {
    char id[40];
    if (s_power.battery_count > 1) snprintf(id, sizeof(id), "bat%u.%s", b, key);
    else snprintf(id, sizeof(id), "bat.%s", key);
    return channel_index(id);
}

static bool power_match() {
    if (!s_power.read) return false;
    if (!filtering()) return true;
    for (u32 i = 0; i < s_power.battery_count; ++i)
        if (battery_hit(i)) return true;
    for (u32 i = 0; i < s_power.rail_count; ++i)
        if (!s_power.rail_cpu[i] && id_hit(s_power.rail_id[i])) return true;
    return false;
}

static void draw_battery(u32 index, bool full) {
    const BatteryInfo* b = &s_power.battery[index];
    const BatteryStatus* s = &b->status;
    const f32 fs = ImGui::GetFontSize();
    char v[64];
    bool first = true;
    if (s->state & BAT_ONLINE) {
        const bool charging = (s->state & BAT_CHARGING) && s->rate_w >= 0.05f;
        stat(&first, "On AC", charging ? "Charging" : "Not charging");
        if (charging) {
            snprintf(v, sizeof(v), "%.1f W", s->rate_w);
            stat(&first, "Charge rate", v, GK_POWER);
        }
    } else {
        stat(&first, "On battery", "Discharging");
        if (!isnan(s->rate_w)) {
            snprintf(v, sizeof(v), "%.1f W", -s->rate_w);
            stat(&first, "Discharge rate", v, GK_POWER);
        }
    }
    theme_help("Charge / discharge rate, voltage and remaining energy as the battery reports them (refreshed by the "
               "firmware every few seconds). On battery the discharge rate is the whole laptop's power draw.");
    if (!isnan(s->charge_pct)) {
        if (!isnan(s->capacity_wh)) snprintf(v, sizeof(v), "%.0f%% (%.1f Wh)", s->charge_pct, s->capacity_wh);
        else snprintf(v, sizeof(v), "%.0f%%", s->charge_pct);
        stat(&first, "Charge", v, GK_LOAD);
    }
    if (!isnan(s->voltage_v)) {
        snprintf(v, sizeof(v), "%.2f V", s->voltage_v);
        stat(&first, "Voltage", v);
    }
    if (!(s->state & BAT_ONLINE) && s->rate_w < -0.5f && !isnan(s->capacity_wh)) {
        const f32 hours = s->capacity_wh / -s->rate_w;
        snprintf(v, sizeof(v), "%u h %02u min", (u32)hours, (u32)(hours * 60.0f) % 60);
        stat(&first, "Time left at this rate", v);
    }

    char line[256];
    snprintf(line, sizeof(line), "%s%s%s%s%s", b->name[0] ? b->name : "Battery", b->manufacturer[0] ? ", " : "",
             b->manufacturer, b->chemistry[0] ? ", " : "", b->chemistry);
    ImGui::TextDisabled("%s", line);
    if (b->design_wh > 0.0f) {
        ImGui::SameLine(0.0f, fs * 1.5f);
        theme_push_numbers();
        ImGui::TextDisabled("design %.1f Wh, full charge %.1f Wh", b->design_wh, b->full_wh);
        if (!isnan(b->wear_pct)) {
            ImGui::SameLine();
            ImGui::TextColored(b->wear_pct >= 20.0f ? g_pal.warn : g_pal.on_surface_variant, "(wear %.1f%%)", b->wear_pct);
        }
        if (b->cycle_count) {
            ImGui::SameLine();
            ImGui::TextDisabled("\xC2\xB7 %u cycles", b->cycle_count);
        }
        ImGui::PopFont();
        theme_help("Wear = how much of its design capacity the battery has lost (full-charge vs design capacity, as the "
                   "battery's own gauge reports them). Cycle count is 0 when the battery doesn't report it.");
    }

    struct Tile { const char* key; const char* title; GraphKind kind; f32 lo, hi; u32 flags; };
    static const Tile tiles[] = {
        { "rate_w", "Rate (+ charging, \xE2\x88\x92 discharging)", GK_POWER, NAN, NAN, GF_ZERO },
        { "charge_pct", "Charge level", GK_LOAD, 0.0f, 100.0f, 0 },
        { "voltage_v", "Voltage", GK_POWER, NAN, NAN, 0 },
        { "temp", "Temperature", GK_TEMP, NAN, NAN, 0 },
    };
    u32 count = 0;
    for (u32 k = 0; k < ARRAY_COUNT(tiles); ++k) {
        const i32 ch = battery_channel(index, tiles[k].key);
        count += ch >= 0 && has_data(&s_ch[ch]) && (!filtering() || channel_hit(&s_ch[ch]));
    }
    if (!count || !grid_begin("##bat_tiles", grid_cols(13.0f, count < 3 ? count : 3))) return;
    for (u32 k = 0; k < ARRAY_COUNT(tiles); ++k) {
        const i32 ch = battery_channel(index, tiles[k].key);
        if (ch < 0 || !has_data(&s_ch[ch]) || (filtering() && !channel_hit(&s_ch[ch]))) continue;
        ImGui::TableNextColumn();
        channel_graph(ch, tiles[k].title, tiles[k].kind, tiles[k].lo, tiles[k].hi, fs * (full ? 5.0f : 4.2f), tiles[k].flags);
    }
    grid_end();
}

// Battery, then the rails that aren't the CPU's (graphics, DRAM, platform; the CPU ones are in the CPU section).
static void draw_power(bool full) {
    const bool filter = filtering();
    const f32 fs = ImGui::GetFontSize();
    section("Power", s_power.battery_count == 1 && s_power.battery[0].name[0] ? s_power.battery[0].name : nullptr);
    if (!s_power.battery_count && !filter) ImGui::TextDisabled("No battery.");
    for (u32 i = 0; i < s_power.battery_count; ++i) {
        if (!battery_hit(i)) continue;
        ImGui::PushID((int)i);
        draw_battery(i, full);
        ImGui::PopID();
    }
    u32 rails = 0;
    for (u32 i = 0; i < s_power.rail_count; ++i) rails += !s_power.rail_cpu[i] && id_hit(s_power.rail_id[i]);
    if (!s_power.rail_count && !filter) {
        ImGui::Spacing();
        ImGui::TextDisabled("Power rails: %s", s_power.emi_status[0] ? s_power.emi_status : "none.");
    }
    if (!rails) return;
    subheading("Power rails");
    theme_help(RAILS_HELP);
    if (!grid_begin("##power_rails", grid_cols(13.0f, 4))) return;
    for (u32 i = 0; i < s_power.rail_count; ++i) {
        if (s_power.rail_cpu[i] || !id_hit(s_power.rail_id[i])) continue;
        ImGui::TableNextColumn();
        channel_graph(channel_index(s_power.rail_id[i]), s_power.rail_name[i], GK_POWER, NAN, NAN, fs * 4.2f, GF_ZERO);
    }
    grid_end();
}

// ---- Overview: device list and sections -------------------------------------------------------------------

// Left-hand list entries (Task Manager style), in display order. The id is saved in settings.ini.
enum DeviceKind : u8 { DEV_ALL, DEV_CPU, DEV_GPU, DEV_MEMORY, DEV_DRIVES, DEV_POWER, DEV_TEMPS, DEV_ADV };

static const char* const ADV_TOPICS[] = { "Advanced sensors PawnIO", "CPU temperatures power voltages", "memory RAM DIMM SPD" };

static bool device_match(DeviceKind k, u32 index, u32 n_read) {
    switch (k) {
        case DEV_ALL: return true;
        case DEV_CPU: return cpu_match();
        case DEV_GPU: return gpu_match(index);
        case DEV_MEMORY: return mem_match();
        case DEV_DRIVES: return group_match(SG_DISK, n_read);
        case DEV_POWER: return power_match();
        case DEV_TEMPS: return group_match(SG_CPU, n_read) || (!filtering() && n_read == 0);
        case DEV_ADV: return !filtering() || filter_hit(ADV_TOPICS, ARRAY_COUNT(ADV_TOPICS));
    }
    return false;
}

static void device_key(DeviceKind k, u32 index, char* out, u32 size) {
    static const char* const keys[] = { "all", "cpu", "gpu", "memory", "drives", "power", "temps", "adv" };
    if (k == DEV_GPU) snprintf(out, size, "gpu%u", index);
    else snprintf(out, size, "%s", keys[k]);
}

static void draw_temps_section(bool full, u32 n_read, f64 age) {
    section("Temperatures", "CPU and system");
    if (!n_read) {
        ImGui::TextDisabled(age < 0.0 ? "Reading sensors\xE2\x80\xA6" : "No temperature sensors found.");
        return;
    }
    if (group_match(SG_CPU, n_read)) draw_temp_tiles(SG_CPU, n_read, ImGui::GetFontSize() * (full ? 6.0f : 4.6f));
    if (filtering()) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Windows' ACPI thermal zones; on most laptops one of them follows the CPU package. Exact per-core "
                        "CPU temperatures and RAM temperatures need a kernel driver, which EzPcBench doesn't install; they "
                        "come from PawnIO when you have installed it (Settings, Advanced sensors).");
    ImGui::PopTextWrapPos();
}

static void draw_drives(bool full, u32 n_read) {
    section("Drives", "temperature");
    draw_temp_tiles(SG_DISK, n_read, ImGui::GetFontSize() * (full ? 6.0f : 4.6f));
    if (!filtering()) ImGui::TextDisabled("Drive readings pause while a disk test runs.");
}

static void draw_device(DeviceKind k, u32 index, bool full, u32 n_read, f64 age) {
    switch (k) {
        case DEV_ALL: break;
        case DEV_CPU: draw_cpu(full, n_read); break;
        case DEV_GPU: draw_gpu(index, full); break;
        case DEV_MEMORY: draw_memory(full); break;
        case DEV_DRIVES: draw_drives(full, n_read); break;
        case DEV_POWER: draw_power(full); break;
        case DEV_TEMPS: draw_temps_section(full, n_read, age); break;
        case DEV_ADV: adv_ui_draw_sensors(); break;
    }
}

// One list entry: thumbnail graph left (v = null: none), title and a one-line summary right. True when clicked.
static bool device_entry(const char* key, const char* title, const char* sub, const f32* v, u32 n, GraphKind kind,
                         f32 lo, f32 hi, bool selected) {
    const f32 fs = ImGui::GetFontSize(), h = fs * 3.1f, pad = fs * 0.35f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const f32 w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(key);
    const bool clicked = ImGui::Selectable("##dev", false, 0, ImVec2(w, h));
    ImGui::PopID();
    if (selected) {     // tinted background with an accent bar in the device's color
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dl->AddRectFilled(a, b, graph_color(kind, theme_is_dark() ? 0.14f : 0.10f), ImGui::GetStyle().FrameRounding);
        dl->AddRectFilled(ImVec2(p.x, a.y), ImVec2(p.x + fs * 0.2f, b.y), graph_color(kind), fs * 0.1f);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad));
    const ImVec2 thumb(fs * 4.2f, h - pad * 2.0f);
    if (v) {
        Graph g;
        g.kind = kind;
        g.flags = GF_SPARK | GF_ZERO;
        g.lo = lo;
        g.hi = hi;
        g.window = win();
        GraphLine line;
        line.y = v;
        line.n = n;
        graph_time(key, &g, &line, 1, thumb);
    } else {
        ImGui::Dummy(thumb);
    }
    ImGui::SameLine(0.0f, fs * 0.6f);
    ImGui::BeginGroup();
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.05f));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme_font_size(0.85f));
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(p.x + w - pad, p.y + h), true);
    ImGui::TextDisabled("%s", sub);
    ImGui::PopClipRect();
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    return clicked;
}

// Hottest reading of group `g` and its channel.
static i32 hottest(SensorGroup g, u32 n_read, f32* temp) {
    i32 best = -1;
    *temp = NAN;
    for (u32 i = 0; i < n_read; ++i)
        if (s_read[i].group == g && s_read[i].valid && !(s_read[i].temp_c <= *temp)) {
            *temp = s_read[i].temp_c;
            best = (i32)i;
        }
    return best >= 0 ? reading_channel(&s_read[best]) : -1;
}

static void draw_device_list(u32 gpus, u32 n_read, DeviceKind* sel, u32* sel_index) {
    const bool filter = filtering();
    char key[16], sub[96], a[32], b[32];
    f32* v = s_h[0];
    auto entry = [&](DeviceKind k, u32 index, const char* title, const char* subtitle, u32 n, GraphKind kind, f32 lo,
                     f32 hi, bool thumb) {
        if (filter && k != DEV_ALL && !device_match(k, index, n_read)) return;
        device_key(k, index, key, sizeof(key));
        const bool selected = filter ? k == DEV_ALL : *sel == k && (k != DEV_GPU || *sel_index == index);
        if (device_entry(key, title, subtitle, thumb ? v : nullptr, n, kind, lo, hi, selected)) {
            *sel = k;
            *sel_index = index;
            snprintf(s_prefs.device, sizeof(s_prefs.device), "%s", key);
            if (filter) s_filter[0] = 0;        // picking a device ends the search
            prefs_save();
        }
    };
    if (filter) snprintf(sub, sizeof(sub), "Matches for \"%s\"", s_filter);
    else snprintf(sub, sizeof(sub), "Every section");
    entry(DEV_ALL, 0, "All", sub, 0, GK_OTHER, NAN, NAN, false);

    u32 n = history(channel_index("cpu.load"), v);
    graph_format(a, sizeof(a), s_cpu.busy_mhz, "MHz");
    snprintf(sub, sizeof(sub), "%.0f%%  %s", s_cpu.avg_util, isnan(s_cpu.busy_mhz) ? "" : a);
    entry(DEV_CPU, 0, "CPU", s_cpu.lps ? sub : "Reading\xE2\x80\xA6", n, GK_LOAD, 0.0f, 100.0f, true);

    for (u32 i = 0; i < gpus; ++i) {
        const GpuLive* g = &s_gpus[i];
        n = history(gpu_channel(i, "load"), v);
        graph_format(a, sizeof(a), g->r.util, "%");
        if (!isnan(g->r.temp_c)) snprintf(sub, sizeof(sub), "%s  %.0f \xC2\xB0" "C", a, g->r.temp_c);
        else snprintf(sub, sizeof(sub), "%s", g->opened ? a : g->name);
        char title[16];
        snprintf(title, sizeof(title), "GPU %u", i);
        entry(DEV_GPU, i, title, sub, n, GK_LOAD, 0.0f, 100.0f, true);
    }

    if (s_mem_count) {
        n = s_mem_count < win() ? s_mem_count : win();
        memcpy(v, s_mem_hist + s_mem_count - n, n * sizeof(f32));
        graph_format(a, sizeof(a), s_mem_hist[s_mem_count - 1], "MiB");
        graph_format(b, sizeof(b), (f32)(s_mem.ullTotalPhys / MiB), "MiB");
        snprintf(sub, sizeof(sub), "%s / %s (%u%%)", a, b, (u32)s_mem.dwMemoryLoad);
        entry(DEV_MEMORY, 0, "Memory", sub, n, GK_MEMORY, 0.0f, (f32)(s_mem.ullTotalPhys / MiB), true);
    }

    f32 t;
    u32 drives = 0;
    for (u32 i = 0; i < n_read; ++i) drives += s_read[i].group == SG_DISK;
    if (drives) {
        n = history(hottest(SG_DISK, n_read, &t), v);
        if (isnan(t)) snprintf(sub, sizeof(sub), "%u drive%s", drives, drives == 1 ? "" : "s");
        else snprintf(sub, sizeof(sub), "%s%.0f \xC2\xB0" "C", drives > 1 ? "hottest " : "", t);
        entry(DEV_DRIVES, 0, "Drives", sub, n, GK_TEMP, NAN, NAN, n > 0);
    }

    if (s_power.read) {
        const i32 ch = s_power.battery_count ? battery_channel(0, "rate_w") : -1;
        n = history(ch, v);
        const BatteryStatus* s = s_power.battery_count ? &s_power.battery[0].status : nullptr;
        if (!s) snprintf(sub, sizeof(sub), "No battery");
        else if (s->state & BAT_ONLINE) snprintf(sub, sizeof(sub), "On AC, %.0f%%", s->charge_pct);
        else if (!isnan(s->rate_w)) snprintf(sub, sizeof(sub), "%.1f W, %.0f%%", -s->rate_w, s->charge_pct);
        else snprintf(sub, sizeof(sub), "On battery");
        entry(DEV_POWER, 0, "Power", sub, n, GK_POWER, NAN, NAN, ch >= 0);
    }

    n = history(hottest(SG_CPU, n_read, &t), v);
    if (isnan(t)) snprintf(sub, sizeof(sub), n_read ? "No reading" : "None found");
    else snprintf(sub, sizeof(sub), "hottest %.0f \xC2\xB0" "C", t);
    entry(DEV_TEMPS, 0, "Temperatures", sub, n, GK_TEMP, NAN, NAN, n > 0);

    entry(DEV_ADV, 0, "Advanced", "PawnIO sensors", 0, GK_OTHER, NAN, NAN, false);
}

static bool device_from_key(const char* key, u32 gpus, DeviceKind* k, u32* index) {
    static const char* const keys[] = { "all", "cpu", "gpu", "memory", "drives", "power", "temps", "adv" };
    *index = 0;
    if (!strncmp(key, "gpu", 3) && key[3] >= '0' && key[3] <= '9') {
        *index = (u32)atoi(key + 3);
        *k = DEV_GPU;
        return *index < gpus;
    }
    for (u32 i = 0; i < ARRAY_COUNT(keys); ++i)
        if (!strcmp(key, keys[i])) { *k = (DeviceKind)i; return *k != DEV_GPU; }
    return false;
}

static void draw_window_tabs() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Graphs show the last");
    for (u32 i = 0; i < ARRAY_COUNT(WINDOW_TABS); ++i) {
        ImGui::SameLine();
        if (theme_tab(WINDOW_TABS[i], s_prefs.window == i)) { s_prefs.window = i; prefs_save(); }
    }
}

static void draw_overview(u32 n_read, f64 age) {
    const u32 gpus = sensors_live(&s_cpu, s_gpus);
    sensors_power(&s_power);
    s_ch_count = sensors_channels(s_ch, CH_MAX);
    const bool filter = filtering();
    DeviceKind sel;
    u32 sel_index;
    if (!device_from_key(s_prefs.device, gpus, &sel, &sel_index)) { sel = DEV_ALL; sel_index = 0; }

    const f32 fs = ImGui::GetFontSize();
    const f32 list_w = fs * 14.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, fs * 0.15f));
    ImGui::BeginChild("##devices", ImVec2(list_w, 0.0f));
    draw_device_list(gpus, n_read, &sel, &sel_index);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SameLine(0.0f, fs * 0.8f);
    ImGui::BeginChild("##detail", ImVec2(0.0f, 0.0f));
    draw_window_tabs();

    if (!filter && sel != DEV_ALL) {
        draw_device(sel, sel_index, true, n_read, age);
        ImGui::EndChild();
        return;
    }
    // All sections (with a search: the ones with a match).
    bool any = false;
    const DeviceKind order[] = { DEV_CPU, DEV_GPU, DEV_MEMORY, DEV_POWER, DEV_DRIVES, DEV_TEMPS, DEV_ADV };
    for (u32 o = 0; o < ARRAY_COUNT(order); ++o) {
        const u32 count = order[o] == DEV_GPU ? gpus : 1;
        for (u32 i = 0; i < count; ++i) {
            if (!device_match(order[o], i, n_read)) continue;
            if (order[o] == DEV_DRIVES && !group_match(SG_DISK, n_read)) continue;
            if (any) {
                ImGui::Spacing();
                ImGui::Separator();
            }
            draw_device(order[o], i, false, n_read, age);
            any = true;
        }
    }
    if (filter && !any) {
        ImGui::Spacing();
        ImGui::TextDisabled("No sensor matches \"%s\".", s_filter);
    }
    ImGui::EndChild();
}

// ---- Sensors tab: every channel -------------------------------------------------------------------------

static f32 s_hist[CH_HISTORY];

static void value_text(const SensorChannel* c, f32 v, bool dim) {
    if (isnan(v)) { dash(); return; }
    const ImVec4 col = dim ? g_pal.on_surface_variant : g_pal.on_surface;
    ImGui::TextColored(col, "%.*f %s", c->decimals > 2 ? 2 : c->decimals, v, c->unit);
}

// In the list: picked ("Picked" view) or not per-core unless asked for ("All"), and matching the search.
static bool channel_listed(const SensorChannel* c) {
    const bool picked = list_has(s_prefs.picked, c->id);
    if (s_prefs.picked_only ? !picked : ((c->flags & CHF_DETAIL) && !s_prefs.detail && !picked)) return false;
    return channel_hit(c);
}

static void draw_channels() {
    const u32 n = s_ch_count = sensors_channels(s_ch, CH_MAX);
    if (theme_button("Reset min / max / avg")) sensors_reset_stats();
    theme_help("Min, max and average start over from the current values.");
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    if (theme_tab("All", !s_prefs.picked_only)) { s_prefs.picked_only = false; prefs_save(); }
    ImGui::SameLine();
    if (theme_tab("Picked", s_prefs.picked_only)) { s_prefs.picked_only = true; prefs_save(); }
    theme_help("Only the sensors ticked in the first column, with ten-minute graphs. The sensor log records these.");
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    if (theme_checkbox("Per-core rows", &s_prefs.detail)) prefs_save();
    theme_help("Also list the clock and load of every logical processor and per-core power rails.");
    if (!n) {
        ImGui::Spacing();
        ImGui::TextDisabled("Reading sensors\xE2\x80\xA6");
        return;
    }
    if (s_prefs.picked_only && !s_prefs.picked[0]) {
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing picked yet: tick sensors in the first column of the \"All\" list.");
        return;
    }
    u32 listed = 0, hidden = 0;
    for (u32 i = 0; i < n; ++i) {
        listed += channel_listed(&s_ch[i]);
        hidden += !s_prefs.picked_only && (s_ch[i].flags & CHF_DETAIL) && !s_prefs.detail && channel_hit(&s_ch[i]);
    }
    if (hidden && filtering()) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize());
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%u more per-core rows match)", hidden);
    }
    if (!listed) {
        ImGui::Spacing();
        ImGui::TextDisabled("No sensor matches \"%s\"%s.", s_filter, s_prefs.picked_only ? " among the picked ones" : "");
        return;
    }

    const f32 fs = ImGui::GetFontSize();
    const bool big = s_prefs.picked_only;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##channels", 8, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Pick", ImGuiTableColumnFlags_WidthFixed, fs * 2.2f);
    ImGui::TableSetupColumn("OSD", ImGuiTableColumnFlags_WidthFixed, fs * 2.2f);
    ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch, big ? 2.0f : 3.2f);
    ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, fs * 5.2f);
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, fs * 5.2f);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, fs * 5.2f);
    ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, fs * 5.2f);
    ImGui::TableSetupColumn(big ? "Last 10 minutes" : "Last 2 minutes", ImGuiTableColumnFlags_WidthStretch, big ? 2.2f : 1.4f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    theme_table_headers();
    ImGui::PopStyleColor();
    for (u32 g = 0; g < CHG_COUNT; ++g) {
        bool header = false;
        for (u32 i = 0; i < n; ++i) {
            const SensorChannel* c = &s_ch[i];
            if (c->group != g || !channel_listed(c)) continue;
            const bool picked = list_has(s_prefs.picked, c->id);
            if (!header) {
                header = true;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(2);
                ImGui::PushFont(g_fonts.medium, 0.0f);
                ImGui::TextUnformatted(g_channel_group_names[g]);
                ImGui::PopFont();
            }
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool v = picked;
            if (theme_checkbox("##pick", &v)) { list_set(s_prefs.picked, sizeof(s_prefs.picked), c->id, v); prefs_save(); }
            theme_help("Pick: show in the \"Picked\" view and record in the sensor log.");
            ImGui::TableNextColumn();
            v = list_has(s_prefs.osd, c->id);
            if (theme_checkbox("##osd", &v)) {
                list_set(s_prefs.osd, sizeof(s_prefs.osd), c->id, v);
                prefs_save();
                if (overlay_visible()) apply_overlay(true);
            }
            theme_help("Show in the on-screen overlay (Log and overlay tab).");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c->name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c->id);
            theme_push_numbers();
            ImGui::TableNextColumn();
            value_text(c, c->valid ? c->value : NAN, false);
            ImGui::TableNextColumn();
            value_text(c, c->min, true);
            ImGui::TableNextColumn();
            value_text(c, c->max, true);
            ImGui::TableNextColumn();
            value_text(c, c->count ? (f32)(c->sum / c->count) : NAN, true);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            const u32 h = sensors_channel_history(i, s_hist, big ? CH_HISTORY : 120);
            if (h > 1) {
                Graph g;
                g.title = c->name;
                g.unit = c->unit;
                g.kind = graph_kind_for_unit(c->unit);
                g.flags = GF_NO_CAPTION;
                if (!strcmp(c->unit, "%")) { g.lo = 0.0f; g.hi = 100.0f; }
                g.min_span = g.kind == GK_TEMP ? 4.0f : 0.0f;
                g.decimals = c->decimals > 2 ? 2 : c->decimals;
                g.window = big ? CH_HISTORY : 120;
                g.step_s = 1.0f;
                GraphLine line;
                line.y = s_hist;
                line.n = h;
                graph_time("##g", &g, &line, 1, ImVec2(0.0f, ImGui::GetTextLineHeight() * (big ? 3.2f : 1.5f)));
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// ---- Log and overlay tab --------------------------------------------------------------------------------

static char s_last_log[520];

static void open_folder_of(const char* path_utf8) {
    wchar_t path[MAX_PATH], args[MAX_PATH + 16];
    MultiByteToWideChar(CP_UTF8, 0, path_utf8, -1, path, MAX_PATH);
    swprintf(args, ARRAY_COUNT(args), L"/select,\"%s\"", path);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args, nullptr, SW_SHOWNORMAL);
}

static void start_log() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    ui_results_dir(dir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(path, MAX_PATH, L"%s\\sensors_%04u%02u%02u_%02u%02u%02u.csv", dir, st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    char err[160];
    if (!sensors_log_start(path, INTERVALS_MS[s_prefs.interval], s_prefs.picked, err, sizeof(err)))
        snprintf(s_last_log, sizeof(s_last_log), "!%s", err);
}

static void stop_log() {
    SensorLogStatus st;
    sensors_log_status(&st);
    sensors_log_stop();
    SensorLogStatus done;
    sensors_log_status(&done);
    if (done.error[0]) snprintf(s_last_log, sizeof(s_last_log), "!%s", done.error);
    else snprintf(s_last_log, sizeof(s_last_log), "%s", st.path);
}

static void draw_log_overlay() {
    SensorLogStatus st;
    sensors_log_status(&st);
    ImGui::Spacing();
    theme_group_begin("Sensor log");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Writes the current value of each sensor at a fixed interval to a CSV file in the results folder "
                        "(opens in a spreadsheet). Logging stops when this window closes.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(st.active);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Interval");
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    int iv = (int)s_prefs.interval;
    if (ImGui::Combo("##interval", &iv, INTERVAL_NAMES, (int)ARRAY_COUNT(INTERVAL_NAMES))) {
        s_prefs.interval = (u32)iv;
        prefs_save();
    }
    ImGui::EndDisabled();
    u32 picked = 0;
    for (const char* p = s_prefs.picked; *p; ++p) picked += *p == ',';
    picked += s_prefs.picked[0] != 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sensors");
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    if (picked) ImGui::Text("the %u picked on the Sensors tab", picked);
    else ImGui::TextUnformatted("all except per-core rows (pick sensors on the Sensors tab to choose)");
    ImGui::Spacing();
    if (!st.active) {
        if (theme_button_primary("Start logging")) start_log();
    } else {
        if (theme_button_primary("Stop logging")) stop_log();
        ImGui::SameLine(0.0f, ImGui::GetFontSize());
        theme_push_numbers();
        const u32 secs = (u32)st.seconds;
        ImGui::Text("%u rows \xC2\xB7 %u sensors \xC2\xB7 %u:%02u", st.rows, st.columns, secs / 60, secs % 60);
        ImGui::PopFont();
        ImGui::TextDisabled("%s", st.path);
        if (st.error[0]) ImGui::TextColored(g_pal.error, "%s", st.error);
    }
    if (!st.active && s_last_log[0]) {
        if (s_last_log[0] == '!') ImGui::TextColored(g_pal.error, "%s", s_last_log + 1);
        else {
            ImGui::TextDisabled("Saved %s", s_last_log);
            ImGui::SameLine();
            if (theme_button("Show in folder")) open_folder_of(s_last_log);
        }
    }
    theme_group_end();

    theme_group_begin("Overlay");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("A small always-on-top panel in a screen corner that clicks pass through. It shows the sensors "
                        "ticked in the OSD column of the Sensors tab, and the frame rate while a GPU test runs. It is an "
                        "ordinary window (nothing is injected into other programs), so it appears over windowed and "
                        "borderless games but not over exclusive fullscreen. It stays open when this window closes.");
    ImGui::PopTextWrapPos();
    bool on = overlay_visible();
    if (theme_checkbox("Show overlay", &on)) apply_overlay(on);
    static const char* const corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Corner");
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    for (u32 i = 0; i < OVL_CORNER_COUNT; ++i) {
        if (i) ImGui::SameLine();
        if (theme_tab(corners[i], s_prefs.corner == i)) {
            s_prefs.corner = (OverlayCorner)i;
            prefs_save();
            if (overlay_visible()) apply_overlay(true);
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Shows");
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    if (!s_prefs.osd[0]) {
        ImGui::TextUnformatted("CPU load and clock, CPU package power, temperatures, GPU load, battery rate (default)");
        theme_group_end();
        return;
    }
    const u32 n = sensors_channels(s_ch, CH_MAX);
    char line[1024] = "";
    for (u32 i = 0; i < n; ++i) {
        if (!list_has(s_prefs.osd, s_ch[i].id)) continue;
        const size_t len = strlen(line);
        snprintf(line + len, sizeof(line) - len, "%s%s", len ? ", " : "", s_ch[i].label);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(line[0] ? line : "(sensors not found on this PC)");
    ImGui::PopTextWrapPos();
    theme_group_end();
}

// ---- window -----------------------------------------------------------------------------------------------

void temps_ui_on_open() {
    prefs_load();
    sensors_start();
    adv_ui_sensors_open();      // PawnIO sampling (no-op without PawnIO)
}

void temps_ui_on_close() {
    if (SensorLogStatus st; sensors_log_status(&st), st.active) stop_log();
    s_filter[0] = 0;
    s_mem_count = 0;
    adv_ui_sensors_close();
    sensors_stop();
}

void temps_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##temps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    apply_view();
    f64 age = -1.0;
    const u32 n = sensors_snapshot(s_read, SENSOR_MAX, &age);
    mem_sample();
    static const char* const tabs[] = { "Overview", "Sensors", "Log and overlay" };
    for (u32 i = 0; i < ARRAY_COUNT(tabs); ++i) {
        if (i) ImGui::SameLine();
        if (theme_tab(tabs[i], s_prefs.tab == i)) { s_prefs.tab = i; prefs_save(); }
    }
    if (s_prefs.tab < 2) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        draw_filter();
    }
    SensorLogStatus st;
    sensors_log_status(&st);
    if (st.active || overlay_visible()) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(g_pal.primary, "%s%s%s", st.active ? "Logging" : "", st.active && overlay_visible() ? " \xC2\xB7 " : "",
                           overlay_visible() ? "Overlay on" : "");
    }
    ImGui::TextDisabled("Live readings, updated every second. Min / max / avg since this window opened or the last reset.");
    ImGui::Separator();
    if (s_prefs.tab == 0) {
        draw_overview(n, age);
    } else if (s_prefs.tab == 1) {
        draw_channels();
    } else {
        draw_log_overlay();
    }
    ImGui::End();
}
