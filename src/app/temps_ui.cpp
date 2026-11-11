// Sensors window: live CPU clock / load per logical processor, GPU clocks / load / VRAM / power / PCIe link,
// then temperatures in one table per group (CPU / system, GPUs, drives); each row shows the current value
// colored against its warning threshold, min / max since the window opened and a two-minute sparkline.
#include "app/temps_ui.h"
#include "app/theme.h"
#include "core/sensors.h"
#include "imgui.h"
#include <math.h>
#include <stdio.h>

static SensorReading s_read[SENSOR_MAX];

// ---- live clocks and load ------------------------------------------------------------------------------

static CpuLive s_cpu;
static GpuLive s_gpus[8];

static void title(const char* t) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

static void ghz_text(f32 mhz) {
    if (isnan(mhz)) ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
    else ImGui::Text("%.2f GHz", mhz / 1000.0f);
}

static void draw_cpu_live() {
    const CpuLive* c = &s_cpu;
    title("CPU clock and load");
    if (!c->lps) {
        ImGui::TextDisabled("Reading\xE2\x80\xA6");
        return;
    }
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

    // Grid of logical processors: "12  4.71 GHz" over a thin load bar.
    const u32 cols = 4;
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("##lps", (int)cols, flags)) return;
    const f32 bar_h = ImGui::GetFontSize() * 0.25f;
    for (u32 i = 0; i < c->lps; ++i) {
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
    if (c->hist_count > 1) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
        ImGui::PlotLines("##cpuload", c->util_hist, (int)c->hist_count, 0, "Load, last 2 minutes", 0.0f, 100.0f,
                         ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 2.5f));
        ImGui::PopStyleColor(2);
    }
}

static const char* pcie_rate(u8 gen) {
    switch (gen) {
        case 1: return "2.5 GT/s"; case 2: return "5 GT/s"; case 3: return "8 GT/s";
        case 4: return "16 GT/s"; case 5: return "32 GT/s"; case 6: return "64 GT/s"; default: return "?";
    }
}

static void draw_gpu_live(u32 n) {
    if (!n) return;
    title("Graphics clocks and load");
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##gpus", 7, flags)) return;
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
        if (isnan(r->util)) ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
        else ImGui::Text("%.0f%%", r->util);
        if (g->hist_count > 1) {
            ImGui::PushStyleColor(ImGuiCol_PlotLines, g_pal.primary);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, g_pal.container);
            ImGui::PlotLines("##load", g->util_hist, (int)g->hist_count, 0, nullptr, 0.0f, 100.0f,
                             ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 1.2f));
            ImGui::PopStyleColor(2);
        }
        ImGui::TableNextColumn();
        if (isnan(r->vram_used_mib)) ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
        else ImGui::Text("%.0f MiB", r->vram_used_mib);
        if (g->vram_total_mib > 0.0f) ImGui::TextDisabled("of %.0f MiB", g->vram_total_mib);
        ImGui::TableNextColumn();
        if (!isnan(r->power_pct)) ImGui::Text("%.0f%%", r->power_pct);
        if (!isnan(r->fan_rpm)) ImGui::TextDisabled("%.0f RPM", r->fan_rpm);
        if (isnan(r->power_pct) && isnan(r->fan_rpm)) ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
        ImGui::TableNextColumn();
        if (r->link_gen && r->link_width) {
            ImGui::Text("Gen %u x%u", r->link_gen, r->link_width);
            if (r->link_gen_max) ImGui::TextDisabled("device: Gen %u x%u", r->link_gen_max, r->link_width_max);
        } else {
            ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
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
}


void temps_ui_on_open() { sensors_start(); }
void temps_ui_on_close() { sensors_stop(); }

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

static void draw_group(SensorGroup g, const char* title, u32 n) {
    u32 count = 0;
    for (u32 i = 0; i < n; ++i) count += s_read[i].group == g;
    if (!count) return;
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(title, 5, flags)) return;
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
        if (s->group != g) continue;
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
            ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
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
}

void temps_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##temps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    f64 age = -1.0;
    const u32 n = sensors_snapshot(s_read, SENSOR_MAX, &age);
    ImGui::TextDisabled("Live readings, updated every second. Min and max since this window opened.");
    ImGui::Separator();
    if (n == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled(age < 0.0 ? "Reading sensors\xE2\x80\xA6" : "No temperature sensors found.");
    }
    const u32 gpus = sensors_live(&s_cpu, s_gpus);
    draw_cpu_live();
    draw_gpu_live(gpus);
    ImGui::Spacing();
    draw_group(SG_CPU, "Temperatures: CPU and system", n);
    draw_group(SG_GPU, "Temperatures: graphics", n);
    draw_group(SG_DISK, "Temperatures: drives", n);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("CPU and system readings are Windows' ACPI thermal zones; on most laptops one of them follows "
                        "the CPU package. Exact per-core CPU temperatures and RAM temperatures need a kernel driver, "
                        "which EzPcBench doesn't install. Drive readings pause while a disk test runs.");
    ImGui::PopTextWrapPos();
    ImGui::End();
}
