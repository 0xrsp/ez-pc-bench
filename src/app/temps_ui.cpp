// Temperatures window: one table per group (CPU / system, GPUs, drives); each row shows the current value
// colored against its warning threshold, min / max since the window opened and a two-minute sparkline.
#include "app/temps_ui.h"
#include "app/theme.h"
#include "core/sensors.h"
#include "imgui.h"
#include <stdio.h>

static SensorReading s_read[SENSOR_MAX];

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
    if (n == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled(age < 0.0 ? "Reading sensors\xE2\x80\xA6" : "No temperature sensors found.");
    }
    draw_group(SG_CPU, "CPU and system", n);
    draw_group(SG_GPU, "Graphics", n);
    draw_group(SG_DISK, "Drives", n);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("CPU and system readings are Windows' ACPI thermal zones; on most laptops one of them follows "
                        "the CPU package. Exact per-core CPU temperatures and RAM temperatures need a kernel driver, "
                        "which EzPcBench doesn't install. Drive readings pause while a disk test runs.");
    ImGui::PopTextWrapPos();
    ImGui::End();
}
