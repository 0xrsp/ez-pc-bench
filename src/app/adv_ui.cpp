// Advanced sensors in the GUI (see adv_ui.h). Histories live here, fed from the core snapshots whenever a new
// sample arrived; the table matches the Sensors window's temperature tables.
#include "app/adv_ui.h"
#include "app/theme.h"
#include "core/adv_sensors.h"
#include "core/sysinfo_spd.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

constexpr u32 HIST = 120;           // samples, one per second
constexpr u32 MAX_SERIES = 48;

struct Series {
    char name[48];
    f32 warn_c;
    f32 now, min, max;
    bool valid;
    f32 hist[HIST];
    u32 count;
};

static Series s_series[MAX_SERIES];
static u32 s_series_count;
static f64 s_last_cpu, s_last_mem;
static AdvCpu s_cpu;
static AdvMem s_mem;

void adv_ui_sensors_open() {
    s_series_count = 0;
    s_last_cpu = s_last_mem = 0.0;
    adv_start();
}

void adv_ui_sensors_close() { adv_stop(); }

static void feed(const char* name, f32 warn_c, f32 v) {
    Series* s = nullptr;
    for (u32 i = 0; i < s_series_count && !s; ++i)
        if (!strcmp(s_series[i].name, name)) s = &s_series[i];
    if (!s) {
        if (isnan(v) || s_series_count >= MAX_SERIES) return;      // rows appear once they have a value
        s = &s_series[s_series_count++];
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->warn_c = warn_c;
    }
    s->valid = !isnan(v);
    if (!s->valid) return;
    if (!s->count) s->min = s->max = v;
    s->now = v;
    s->min = v < s->min ? v : s->min;
    s->max = v > s->max ? v : s->max;
    if (s->count < HIST) s->hist[s->count++] = v;
    else {
        memmove(s->hist, s->hist + 1, (HIST - 1) * sizeof(f32));
        s->hist[HIST - 1] = v;
    }
}

static void update() {
    char name[48];
    if (adv_cpu_snapshot(&s_cpu) && s_cpu.time != s_last_cpu) {
        s_last_cpu = s_cpu.time;
        const bool amd = !isnan(s_cpu.tctl_c);
        feed(amd ? "CPU (Tdie)" : "CPU package", 95.0f, s_cpu.temp_c);
        if (amd && fabsf(s_cpu.tctl_c - s_cpu.temp_c) > 0.05f) feed("CPU (Tctl, fan control)", 95.0f, s_cpu.tctl_c);
        for (u32 i = 0; i < s_cpu.ccd_count; ++i) {
            snprintf(name, sizeof(name), "CCD %u", i + 1);
            feed(name, 90.0f, s_cpu.ccd_c[i]);
        }
        for (u32 i = 0; i < s_cpu.core_count; ++i) {
            snprintf(name, sizeof(name), "Core %u", i);
            feed(name, 95.0f, s_cpu.core_c[i]);
        }
    }
    if (adv_mem_snapshot(&s_mem) && s_mem.time != s_last_mem) {
        s_last_mem = s_mem.time;
        for (u32 i = 0; i < s_mem.count; ++i) {
            snprintf(name, sizeof(name), "DIMM at 0x%02X (%s)", s_mem.dimms[i].addr, s_mem.dimms[i].ddr5 ? "SPD hub" : "TSE2004");
            feed(name, 85.0f, s_mem.dimms[i].valid ? s_mem.dimms[i].temp_c : NAN);
        }
    }
}

static void temp_text(f32 c, ImVec4 col) {
    theme_push_numbers();
    ImGui::TextColored(col, "%.1f \xC2\xB0" "C", c);
    ImGui::PopFont();
}

static void draw_table() {
    if (!s_series_count) return;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##advtemps", 5, flags)) return;
    const f32 fs = ImGui::GetFontSize(), line = ImGui::GetTextLineHeight();
    ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, fs * 5.5f);
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Last 2 minutes", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    for (u32 i = 0; i < s_series_count; ++i) {
        const Series* s = &s_series[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s->name);
        ImGui::TableNextColumn();
        if (s->valid) {
            const ImVec4 col = s->now >= s->warn_c ? g_pal.error : s->now >= s->warn_c - 10.0f ? g_pal.warn : g_pal.on_surface;
            temp_text(s->now, col);
        } else {
            ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
        }
        ImGui::TableNextColumn();
        if (s->count) temp_text(s->min, g_pal.on_surface_variant);
        ImGui::TableNextColumn();
        if (s->count) temp_text(s->max, g_pal.on_surface_variant);
        ImGui::TableNextColumn();
        if (s->count > 1) {
            f32 lo = s->min - 2.0f, hi = s->max + 2.0f;
            if (hi - lo < 10.0f) { const f32 mid = (hi + lo) * 0.5f; lo = mid - 5.0f; hi = mid + 5.0f; }
            ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
            ImGui::PlotLines("##hist", s->hist, (int)s->count, 0, nullptr, lo, hi, ImVec2(-FLT_MIN, line * 2.0f));
            ImGui::PopStyleColor(2);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// "label value unit" pairs on one line, skipping NaN values; returns true if anything was drawn.
struct Field { const char* label; f32 v; const char* fmt; };
static bool field_line(const Field* f, u32 n) {
    char buf[256] = "";
    size_t len = 0;
    for (u32 i = 0; i < n; ++i) {
        if (isnan(f[i].v) || len + 2 >= sizeof(buf)) continue;
        if (len) len += snprintf(buf + len, sizeof(buf) - len, "  \xC2\xB7  ");
        len += snprintf(buf + len, sizeof(buf) - len, "%s ", f[i].label);
        len += snprintf(buf + len, sizeof(buf) - len, f[i].fmt, f[i].v);
    }
    if (!len) return false;
    theme_push_numbers();
    ImGui::TextUnformatted(buf);
    ImGui::PopFont();
    return true;
}

static void draw_values(const AdvInfo* info) {
    const AdvCpu* c = &s_cpu;
    const Field power[] = { { "Package", c->package_w, "%.1f W" }, { "cores", c->cores_w, "%.1f W" },
                            { "graphics", c->gfx_w, "%.1f W" }, { "SMU PPT", c->ppt_w, "%.1f W" } };
    const Field volts[] = { { "Vcore", c->vcore_v, "%.3f V" }, { "VSoC", c->vsoc_v, "%.3f V" } };
    const Field clocks[] = { { "FCLK", c->fclk_mhz, "%.0f MHz" }, { "UCLK", c->uclk_mhz, "%.0f MHz" },
                             { "MCLK", c->mclk_mhz, "%.0f MHz" } };
    field_line(power, ARRAY_COUNT(power));
    field_line(volts, ARRAY_COUNT(volts));
    field_line(clocks, ARRAY_COUNT(clocks));

    // Per-core PM table clocks and APERF/MPERF clocks: summary line, all values on hover.
    f32 sum = 0.0f, hi = NAN;
    u32 n = 0;
    for (u32 i = 0; i < c->lp_count; ++i)
        if (!isnan(c->lp_mhz[i]) && c->lp_busy[i] >= 50.0f) { sum += c->lp_mhz[i]; ++n; }
    for (u32 i = 0; i < c->lp_count; ++i)
        if (!isnan(c->lp_mhz[i]) && !(c->lp_mhz[i] <= hi)) hi = c->lp_mhz[i];
    if (c->lp_count) {
        theme_push_numbers();
        if (n) ImGui::Text("APERF/MPERF: busy cores %.2f GHz, highest %.2f GHz", sum / n / 1000.0f, hi / 1000.0f);
        else if (!isnan(hi)) ImGui::Text("APERF/MPERF: no core busy, highest %.2f GHz", hi / 1000.0f);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            theme_push_numbers();
            for (u32 i = 0; i < c->lp_count; ++i)
                if (!isnan(c->lp_mhz[i])) ImGui::Text("LP %2u  %5.0f MHz  %3.0f%% in C0", i, c->lp_mhz[i], c->lp_busy[i]);
            ImGui::PopFont();
            ImGui::EndTooltip();
        }
    } else if (s_last_cpu > 0.0) {
        ImGui::TextDisabled("Per-core clocks pause while a benchmark runs.");
    }
    if (!isnan(c->stretch_pct)) {
        ImGui::Text("Clock stretching %.1f%%", c->stretch_pct > 0.0f ? c->stretch_pct : 0.0f);
        theme_help("Busiest core's effective clock (APERF/MPERF) against the clock its current P-state asks for. A few "
                   "percent is normal measurement noise; more means the core is stretching its clock (voltage droop, "
                   "often from too aggressive an undervolt / Curve Optimizer).");
    }
    if (c->limits) {
        char lim[128] = "";
        static const char* const names[] = { "thermal", "PROCHOT", "power", "current", "cross-domain", "critical temperature" };
        for (u32 i = 0; i < ARRAY_COUNT(names); ++i)
            if (c->limits & (1u << i)) {
                const size_t len = strlen(lim);
                snprintf(lim + len, sizeof(lim) - len, "%s%s", len ? ", " : "", names[i]);
            }
        ImGui::TextColored(g_pal.warn, "Limiting now: %s", lim);
    }
    char t[192];
    if (memdetail_timings_line(t, sizeof(t))) ImGui::TextDisabled("Memory: %s", t);
    if (info->pm_table_version)
        ImGui::TextDisabled("SMU PM table 0x%06X%s", info->pm_table_version,
                            info->pm_table_mapped ? "" : " (layout not mapped yet: run the CLI with --adv-report to dump it)");
    if (info->smbus_note[0]) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("DIMM sensors: %s", info->smbus_note);
        ImGui::PopTextWrapPos();
    }
}

void adv_ui_draw_sensors() {
    AdvInfo info;
    const AdvStatus st = adv_status(&info);
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted("Advanced sensors (PawnIO)");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    if (st != ADV_ACTIVE) {
        ImGui::TextDisabled("%s. %s", adv_status_text(st), info.detail);
        ImGui::PopTextWrapPos();
        return;
    }
    update();
    if (info.detail[0]) ImGui::TextDisabled("%s", info.detail);
    if (!s_last_cpu) ImGui::TextDisabled("Reading\xE2\x80\xA6");
    draw_table();
    draw_values(&info);
    ImGui::PopTextWrapPos();
}

const char* adv_ui_settings_help() {
    static char help[640];
    AdvInfo info;
    const AdvStatus st = adv_status(&info);
    snprintf(help, sizeof(help),
             "Per-core and package CPU temperatures, CPU power, voltages, memory temperatures, SPD and timings through "
             "PawnIO, a signed open-source driver used by LibreHardwareMonitor, FanControl and others. EzPcBench never "
             "installs it: get the official installer from %s. Without it everything else works as before.\n\n"
             "Status: %s. %s%s%s",
             ADV_PAWNIO_URL, adv_status_text(st), info.detail, info.modules[0] ? "\nModules: " : "", info.modules);
    return help;
}

void adv_ui_settings_control() {
    AdvInfo info;
    const AdvStatus st = adv_status(&info);
    if (st == ADV_NOT_INSTALLED) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Not installed");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", ADV_PAWNIO_URL);
        ImGui::SameLine();
        if (theme_button("Copy link")) ImGui::SetClipboardText(ADV_PAWNIO_URL);
        return;
    }
    bool on = adv_enabled();
    if (theme_checkbox("##adv", &on)) adv_set_enabled(on);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (st == ADV_ACTIVE) ImGui::TextUnformatted("On");
    else ImGui::TextDisabled("%s", st == ADV_OFF ? "Off" : adv_status_text(st));
}
