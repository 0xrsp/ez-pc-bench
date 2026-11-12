// Sensors window, three tabs:
//   Overview          live CPU clock / load per logical processor and CPU power rails, GPU clocks / load / VRAM /
//                     power / PCIe link, battery and other power rails, then temperatures in one table per group
//                     (CPU / system, GPUs, drives) with min / max since the window opened and a two-minute sparkline.
//   Sensors           every channel (core/sensors.h) with now / min / max / avg (resettable) and a graph;
//                     pick channels for the log and the "Picked" view, tick channels for the overlay.
//   Log and overlay   sensor log to CSV at a chosen interval; the on-screen overlay.
// A search box filters the first two tabs (not saved). Picks, overlay channels and log / overlay options
// persist in settings.ini [sensors].
#include "app/temps_ui.h"
#include "app/adv_ui.h"
#include "app/overlay.h"
#include "app/settings.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/sensors.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
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
};
static Prefs s_prefs = { "", "", 2, OVL_TOP_RIGHT, 0, false, false };
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

static void prefs_load() {
    if (s_prefs_loaded) return;
    s_prefs_loaded = true;
    const wchar_t* ini = settings_ini_file();
    if (!ini) return;
    pref_read_str(ini, L"picked", s_prefs.picked, sizeof(s_prefs.picked));
    pref_read_str(ini, L"overlay", s_prefs.osd, sizeof(s_prefs.osd));
    s_prefs.interval = pref_read_u32(ini, L"log_interval", 2, ARRAY_COUNT(INTERVALS_MS) - 1);
    s_prefs.corner = (OverlayCorner)pref_read_u32(ini, L"overlay_corner", OVL_TOP_RIGHT, OVL_CORNER_COUNT - 1);
    s_prefs.tab = pref_read_u32(ini, L"tab", 0, 2);
    s_prefs.picked_only = pref_read_u32(ini, L"picked_only", 0, 1) != 0;
    s_prefs.detail = pref_read_u32(ini, L"per_core", 0, 1) != 0;
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
    const struct { const wchar_t* key; u32 v; } nums[] = {
        { L"log_interval", s_prefs.interval }, { L"overlay_corner", s_prefs.corner },
        { L"tab", s_prefs.tab }, { L"picked_only", s_prefs.picked_only }, { L"per_core", s_prefs.detail },
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

static const SensorChannel* channel_by_id(const char* id) {
    for (u32 i = 0; i < s_ch_count; ++i)
        if (!strcmp(s_ch[i].id, id)) return &s_ch[i];
    return nullptr;
}

static bool id_hit(const char* id) {
    if (!filtering()) return true;
    const SensorChannel* c = channel_by_id(id);
    return c && channel_hit(c);
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

static void title(const char* t) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

static void dash() { ImGui::TextColored(g_pal.outline, "\xE2\x80\x94"); }

static void ghz_text(f32 mhz) {
    if (isnan(mhz)) dash();
    else ImGui::Text("%.2f GHz", mhz / 1000.0f);
}

// Line graph of `n` values (NaN = gap) filling `size`; y range from the data (pct: fixed 0..100).
static void spark(const f32* v, u32 n, ImVec2 size, bool pct) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(g_pal.container), 3.0f);
    f32 lo = INFINITY, hi = -INFINITY;
    for (u32 i = 0; i < n; ++i)
        if (!isnan(v[i])) { lo = v[i] < lo ? v[i] : lo; hi = v[i] > hi ? v[i] : hi; }
    if (lo > hi) return;
    if (pct) { lo = 0.0f; hi = 100.0f; }
    else {
        const f32 span = hi - lo, pad = span > 0.0f ? span * 0.15f : (fabsf(hi) > 1.0f ? fabsf(hi) * 0.05f : 0.5f);
        lo -= pad;
        hi += pad;
    }
    const ImU32 col = ImGui::GetColorU32(g_pal.primary);
    const f32 dx = n > 1 ? size.x / (f32)(n - 1) : 0.0f, inset = 2.0f;
    ImVec2 prev;
    bool have = false;
    for (u32 i = 0; i < n; ++i) {
        if (isnan(v[i])) { have = false; continue; }
        const f32 t = (v[i] - lo) / (hi - lo);
        const ImVec2 p(p0.x + dx * i, p0.y + inset + (size.y - 2.0f * inset) * (1.0f - (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t)));
        if (have) dl->AddLine(prev, p, col, 1.5f);
        prev = p;
        have = true;
    }
    if (ImGui::IsItemHovered() && !pct)
        ImGui::SetTooltip("Range shown: %.1f \xE2\x80\x93 %.1f", lo, hi);
}

// ---- Overview: live clocks, load and CPU power ----------------------------------------------------------
// Each draw_* returns whether it drew anything (with a search active, sections without a match are skipped).

static const char* const RAILS_HELP = "Energy Metering Interface: power rails the platform exposes to Windows (here "
                                      "RAPL-style energy counters). Averaged over the last second.";

static bool core_rail(u32 i) { return !strncmp(s_power.rail_name[i], "CPU core ", 9); }

static void rail_text(u32 i) {
    theme_push_numbers();
    if (isnan(s_power.rail_w[i])) ImGui::Text("%s \xE2\x80\x94", s_power.rail_name[i]);
    else ImGui::Text("%s %.1f W", s_power.rail_name[i], s_power.rail_w[i]);
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Energy meter channel \"%s\" (%s)", s_power.rail_raw[i], s_power.rail_id[i]);
}

// Spacing between items on one line; the first item starts the line.
static void next_item(bool* first) {
    if (!*first) ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
    *first = false;
}

static bool lp_hit(u32 i) {
    char a[16], b[16];
    snprintf(a, sizeof(a), "lp%u.mhz", i);
    snprintf(b, sizeof(b), "lp%u.load", i);
    return id_hit(a) || id_hit(b);
}

// Per-core power rails in a grid: "Core 3  2.15 W".
static void draw_core_rails() {
    if (!ImGui::BeginTable("##rails", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) return;
    for (u32 i = 0; i < s_power.rail_count; ++i) {
        if (!s_power.rail_cpu[i] || !core_rail(i) || !id_hit(s_power.rail_id[i])) continue;
        ImGui::TableNextColumn();
        theme_push_numbers();
        ImGui::TextDisabled("Core %s", s_power.rail_name[i] + 9);
        ImGui::SameLine();
        if (isnan(s_power.rail_w[i])) dash();
        else ImGui::Text("%.2f W", s_power.rail_w[i]);
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

static bool draw_cpu_live() {
    const CpuLive* c = &s_cpu;
    const bool summary = id_hit("cpu.load") || id_hit("cpu.busy_mhz") || id_hit("cpu.max_mhz");
    u32 lps = 0, rails = 0, cores = 0, all_rails = 0;
    for (u32 i = 0; i < c->lps; ++i) lps += lp_hit(i);
    for (u32 i = 0; i < s_power.rail_count; ++i) {
        if (!s_power.rail_cpu[i]) continue;
        ++all_rails;
        if (id_hit(s_power.rail_id[i])) ++(core_rail(i) ? cores : rails);
    }
    if (!summary && !lps && !rails && !cores) return false;
    title(all_rails ? "CPU clock, load and power" : "CPU clock and load");
    if (!c->lps) {
        ImGui::TextDisabled("Reading\xE2\x80\xA6");
        return true;
    }
    bool first = true;
    if (summary) {
        next_item(&first);
        theme_push_numbers();
        ImGui::Text("Load %.0f%%", c->avg_util);
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        if (!isnan(c->busy_mhz)) ImGui::Text("Busy cores %.2f GHz", c->busy_mhz / 1000.0f);
        else ImGui::TextDisabled("No core busy");
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        if (!isnan(c->max_mhz)) ImGui::TextDisabled("Highest seen %.2f GHz", c->max_mhz / 1000.0f);
        ImGui::PopFont();
        theme_help("Effective clock per logical processor (Windows' APERF/MPERF-based \"% Processor Performance\" x the "
                   "nominal clock) and its load over the last second. Idle cores report parked clocks; \"Busy cores\" "
                   "averages only the ones at least half busy. Under full load these readings can lag by a second.");
    }
    for (u32 i = 0; i < s_power.rail_count; ++i) {      // package / all-cores rails on the same line
        if (!s_power.rail_cpu[i] || core_rail(i) || !id_hit(s_power.rail_id[i])) continue;
        next_item(&first);
        rail_text(i);
    }
    if (rails) theme_help(RAILS_HELP);

    // Grid of logical processors: "12  4.71 GHz" over a thin load bar.
    const u32 cols = 4;
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX;
    if (lps && ImGui::BeginTable("##lps", (int)cols, flags)) {
        const f32 bar_h = ImGui::GetFontSize() * 0.25f;
        for (u32 i = 0; i < c->lps; ++i) {
            if (!lp_hit(i)) continue;
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();   // same baseline in every column (the bars below set the row's)
            theme_push_numbers();
            ImGui::TextDisabled("%2u", i);
            ImGui::SameLine();
            ghz_text(c->mhz[i]);
            ImGui::SameLine();
            ImGui::TextDisabled("%3.0f%%", c->util[i]);
            ImGui::PopFont();
            theme_progress(c->util[i] / 100.0f, bar_h, ImGui::GetContentRegionAvail().x * 0.92f);
        }
        ImGui::EndTable();
    }
    if (cores) {
        ImGui::TextDisabled("Power per core");
        theme_help(RAILS_HELP);
        draw_core_rails();
    }
    if (summary && c->hist_count > 1) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
        ImGui::PlotLines("##cpuload", c->util_hist, (int)c->hist_count, 0, "Load, last 2 minutes", 0.0f, 100.0f,
                         ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.5f));
        ImGui::PopStyleColor(2);
    }
    return true;
}

static const char* pcie_rate(u8 gen) {
    switch (gen) {
        case 1: return "2.5 GT/s"; case 2: return "5 GT/s"; case 3: return "8 GT/s";
        case 4: return "16 GT/s"; case 5: return "32 GT/s"; case 6: return "64 GT/s"; default: return "?";
    }
}

static bool gpu_hit(u32 i) {
    static const char* const keys[] = { "core_mhz", "mem_mhz", "load", "vram_mib", "power_pct", "fan_rpm" };
    char id[40];
    for (u32 k = 0; k < ARRAY_COUNT(keys); ++k) {
        snprintf(id, sizeof(id), "gpu%u.%s", i, keys[k]);
        if (id_hit(id)) return true;
    }
    return false;
}

static bool draw_gpu_live(u32 n) {
    u32 shown = 0;
    for (u32 i = 0; i < n; ++i) shown += gpu_hit(i);
    if (!shown) return false;
    title("Graphics clocks and load");
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##gpus", 7, flags)) return true;
    ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("Core", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("VRAM used", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Power / fan", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("PCIe link", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    for (u32 i = 0; i < n; ++i) {
        if (!gpu_hit(i)) continue;
        const GpuLive* g = &s_gpus[i];
        const GpuReading* r = &g->r;
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(g->name);
        theme_push_numbers();
        ImGui::TableNextColumn();
        ghz_text(r->core_mhz);
        if (!isnan(r->core_max_mhz)) ImGui::TextDisabled("max %.2f", r->core_max_mhz / 1000.0f);
        ImGui::TableNextColumn();
        ghz_text(r->mem_mhz);
        if (!isnan(r->mem_max_mhz)) ImGui::TextDisabled("max %.2f", r->mem_max_mhz / 1000.0f);
        ImGui::TableNextColumn();
        if (isnan(r->util)) dash();
        else ImGui::Text("%.0f%%", r->util);
        if (g->hist_count > 1) {
            ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
            ImGui::PlotLines("##load", g->util_hist, (int)g->hist_count, 0, nullptr, 0.0f, 100.0f,
                             ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 1.2f));
            ImGui::PopStyleColor(2);
        }
        ImGui::TableNextColumn();
        if (isnan(r->vram_used_mib)) dash();
        else ImGui::Text("%.0f MiB", r->vram_used_mib);
        if (g->vram_total_mib > 0.0f) ImGui::TextDisabled("of %.0f MiB", g->vram_total_mib);
        ImGui::TableNextColumn();
        if (!isnan(r->power_pct)) ImGui::Text("%.0f%%", r->power_pct);
        if (!isnan(r->fan_rpm)) ImGui::TextDisabled("%.0f RPM", r->fan_rpm);
        if (isnan(r->power_pct) && isnan(r->fan_rpm)) dash();
        ImGui::TableNextColumn();
        if (r->link_gen && r->link_width) {
            ImGui::Text("Gen %u x%u", r->link_gen, r->link_width);
            if (r->link_gen_max) ImGui::TextDisabled("device: Gen %u x%u", r->link_gen_max, r->link_width_max);
        } else {
            dash();
        }
        ImGui::PopFont();
        if (r->link_gen && ImGui::IsItemHovered())
            ImGui::SetTooltip("Link now: %s x%u. The GPU itself supports %s x%u; the slot or the laptop's wiring may "
                              "allow less. GPUs drop to a slower link when idle to save power and speed it up under load.",
                              pcie_rate(r->link_gen), r->link_width, pcie_rate(r->link_gen_max), r->link_width_max);
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Clocks the driver doesn't report show \xE2\x80\x94 (e.g. a laptop GPU that is asleep). Power is "
                        "the share of the GPU's power limit.");
    return true;
}

// ---- Overview: battery and the other power rails --------------------------------------------------------

static void draw_battery(const BatteryInfo* b) {
    const BatteryStatus* s = &b->status;
    theme_push_numbers();
    if (s->state & BAT_ONLINE) {
        if ((s->state & BAT_CHARGING) && s->rate_w >= 0.05f) ImGui::Text("On AC, charging at %.1f W", s->rate_w);
        else ImGui::TextUnformatted("On AC, not charging");
    } else if (!isnan(s->rate_w)) {
        ImGui::Text("Discharging at %.1f W", -s->rate_w);
    } else {
        ImGui::TextUnformatted("On battery");
    }
    if (!isnan(s->charge_pct)) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        if (!isnan(s->capacity_wh)) ImGui::Text("%.0f%% (%.1f Wh)", s->charge_pct, s->capacity_wh);
        else ImGui::Text("%.0f%%", s->charge_pct);
    }
    if (!isnan(s->voltage_v)) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        ImGui::Text("%.2f V", s->voltage_v);
    }
    if (!(s->state & BAT_ONLINE) && s->rate_w < -0.5f && !isnan(s->capacity_wh)) {
        const f32 hours = s->capacity_wh / -s->rate_w;
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 2.0f);
        ImGui::TextDisabled("~%u h %02u min left at this rate", (u32)hours, (u32)(hours * 60.0f) % 60);
    }
    ImGui::PopFont();
    theme_help("Charge / discharge rate, voltage and remaining energy as the battery reports them (refreshed by the "
               "firmware every few seconds). On battery the discharge rate is the whole laptop's power draw.");
    char line[256];
    snprintf(line, sizeof(line), "%s%s%s%s%s", b->name[0] ? b->name : "Battery", b->manufacturer[0] ? ", " : "",
             b->manufacturer, b->chemistry[0] ? ", " : "", b->chemistry);
    ImGui::TextDisabled("%s", line);
    if (b->design_wh > 0.0f) {
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
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
}

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

// Battery, then the rails that aren't the CPU's (graphics, DRAM, platform; the CPU ones are in the CPU section).
static bool draw_power() {
    if (!s_power.read) return false;
    const bool filter = filtering();
    u32 bats = 0, rails = 0;
    for (u32 i = 0; i < s_power.battery_count; ++i) bats += battery_hit(i);
    for (u32 i = 0; i < s_power.rail_count; ++i) rails += !s_power.rail_cpu[i] && id_hit(s_power.rail_id[i]);
    if (filter && !bats && !rails) return false;
    title("Power");
    if (!s_power.battery_count && !filter) ImGui::TextDisabled("No battery.");
    for (u32 i = 0; i < s_power.battery_count; ++i) {
        if (!battery_hit(i)) continue;
        ImGui::PushID((int)i);
        draw_battery(&s_power.battery[i]);
        ImGui::PopID();
    }
    if (!s_power.rail_count && !filter) {
        ImGui::Spacing();
        ImGui::TextDisabled("Power rails: %s", s_power.emi_status[0] ? s_power.emi_status : "none.");
    }
    if (!rails) return true;
    ImGui::Spacing();
    bool first = true;
    for (u32 i = 0; i < s_power.rail_count; ++i) {
        if (s_power.rail_cpu[i] || !id_hit(s_power.rail_id[i])) continue;
        next_item(&first);
        rail_text(i);
    }
    theme_help(RAILS_HELP);
    return true;
}

// ---- Overview: temperatures -----------------------------------------------------------------------------

static ImVec4 temp_color(const SensorReading* s) {
    if (s->warn_c > 0.0f && s->temp_c >= s->warn_c) return g_pal.error;
    if (s->warn_c > 0.0f && s->temp_c >= s->warn_c - 10.0f) return g_pal.warn;
    return g_pal.on_surface;
}

static void temp_text(f32 c, ImVec4 col) {
    theme_push_numbers();
    ImGui::TextColored(col, "%.1f \xC2\xB0" "C", c);
    ImGui::PopFont();
}

// The reading's channel ("temp.tz01", "gpu0.temp", "disk1.temp": named "<reading>" or "<reading> temperature").
static bool reading_hit(const SensorReading* s) {
    if (!filtering()) return true;
    char with[128];
    snprintf(with, sizeof(with), "%s temperature", s->name);
    for (u32 i = 0; i < s_ch_count; ++i)
        if (s_ch[i].group == CHG_TEMP && (!strcmp(s_ch[i].name, s->name) || !strcmp(s_ch[i].name, with)))
            return channel_hit(&s_ch[i]);
    const char* t[] = { s->name, s->detail, g_channel_group_names[CHG_TEMP] };
    return filter_hit(t, ARRAY_COUNT(t));
}

static bool draw_group(SensorGroup g, const char* group_title, u32 n) {
    u32 count = 0;
    for (u32 i = 0; i < n; ++i) count += s_read[i].group == g && reading_hit(&s_read[i]);
    if (!count) return false;
    title(group_title);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(group_title, 5, flags)) return true;
    const f32 fs = ImGui::GetFontSize();
    ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, fs * 5.5f);
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Last 2 minutes", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    const f32 line = ImGui::GetTextLineHeight();
    for (u32 i = 0; i < n; ++i) {
        const SensorReading* s = &s_read[i];
        if (s->group != g || !reading_hit(s)) continue;
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s->name);
        if (s->detail[0]) ImGui::TextDisabled("%s", s->detail);
        ImGui::TableNextColumn();
        if (s->valid) {
            temp_text(s->temp_c, temp_color(s));
            char tip[128];
            snprintf(tip, sizeof(tip), "Warning threshold: %.0f \xC2\xB0" "C", s->warn_c);
            theme_help(tip);
        } else {
            dash();
            theme_help(s->note[0] ? s->note : "No reading yet.");
        }
        ImGui::TableNextColumn();
        if (s->history_count) temp_text(s->min_c, g_pal.on_surface_variant);
        ImGui::TableNextColumn();
        if (s->history_count) temp_text(s->max_c, g_pal.on_surface_variant);
        ImGui::TableNextColumn();
        if (s->history_count > 1) {
            f32 lo = s->min_c - 2.0f, hi = s->max_c + 2.0f;
            if (hi - lo < 10.0f) { const f32 mid = (hi + lo) * 0.5f; lo = mid - 5.0f; hi = mid + 5.0f; }
            ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
            ImGui::PlotLines("##hist", s->history, (int)s->history_count, 0, nullptr, lo, hi,
                             ImVec2(-FLT_MIN, line * 2.0f));
            ImGui::PopStyleColor(2);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    return true;
}

static void draw_overview(u32 n, f64 age) {
    const bool filter = filtering();
    if (n == 0 && !filter) {
        ImGui::Spacing();
        ImGui::TextDisabled(age < 0.0 ? "Reading sensors\xE2\x80\xA6" : "No temperature sensors found.");
    }
    const u32 gpus = sensors_live(&s_cpu, s_gpus);
    sensors_power(&s_power);
    s_ch_count = sensors_channels(s_ch, CH_MAX);
    bool any = draw_cpu_live();
    any |= draw_gpu_live(gpus);
    any |= draw_power();
    ImGui::Spacing();
    any |= draw_group(SG_CPU, "Temperatures: CPU and system", n);
    any |= draw_group(SG_GPU, "Temperatures: graphics", n);
    any |= draw_group(SG_DISK, "Temperatures: drives", n);
    // The PawnIO section (adv_ui) isn't filtered row by row: shown when the search matches what it reads.
    static const char* const adv[] = { "Advanced sensors PawnIO", "CPU temperatures power voltages", "memory RAM DIMM SPD" };
    if (!filter || filter_hit(adv, ARRAY_COUNT(adv))) {
        adv_ui_draw_sensors();
        any = true;
    }
    if (filter) {
        if (!any) {
            ImGui::Spacing();
            ImGui::TextDisabled("No sensor matches \"%s\".", s_filter);
        }
        return;
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("CPU and system readings are Windows' ACPI thermal zones; on most laptops one of them follows "
                        "the CPU package. Exact per-core CPU temperatures and RAM temperatures need a kernel driver, "
                        "which EzPcBench doesn't install; they come from PawnIO when you have installed it (Settings, "
                        "Advanced sensors). Drive readings pause while a disk test runs.");
    ImGui::PopTextWrapPos();
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
    ImGui::TableHeadersRow();
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
            if (h > 1)
                spark(s_hist, h, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight() * (big ? 3.0f : 1.2f)),
                      !strcmp(c->unit, "%"));
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
    title("Sensor log");
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

    ImGui::Spacing();
    ImGui::Spacing();
    title("Overlay");
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
    adv_ui_sensors_close();
    sensors_stop();
}

void temps_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##temps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    f64 age = -1.0;
    const u32 n = sensors_snapshot(s_read, SENSOR_MAX, &age);
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
        ImGui::BeginChild("##overview", ImVec2(0, 0));
        draw_overview(n, age);
        ImGui::EndChild();
    } else if (s_prefs.tab == 1) {
        draw_channels();
    } else {
        draw_log_overlay();
    }
    ImGui::End();
}
