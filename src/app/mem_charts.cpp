// Memory result views for the Charts tab (see mem_charts.h).
#include "app/mem_charts.h"
#include "app/theme.h"
#include "core/results.h"
#include "imgui.h"
#include <stdio.h>
#include <string.h>

// Rows L1 / L2 / L3 / Memory, columns Read / Write / Copy / Latency, from metrics named "<row> <column>".
static void draw_grid(const BenchResult* r) {
    static const char* const rows[] = { "L1", "L2", "L3", "Memory" };
    static const char* const cols[] = { "read", "write", "copy", "latency" };
    static const char* const heads[] = { "Read", "Write", "Copy", "Latency" };
    const Metric* m = res_metrics(r);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
    const f32 w = ImGui::GetFontSize() * 7.0f;
    if (!ImGui::BeginTable("grid", 5, flags)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.0f);
    for (u32 c = 0; c < 4; ++c) ImGui::TableSetupColumn(heads[c], ImGuiTableColumnFlags_WidthFixed, w);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (u32 c = 0; c < 5; ++c) {
        ImGui::TableSetColumnIndex((int)c);
        const char* name = c ? heads[c - 1] : "";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(name).x);
        ImGui::TextUnformatted(name);
    }
    ImGui::PopStyleColor();
    char name[32], v[48];
    for (u32 row = 0; row < ARRAY_COUNT(rows); ++row) {
        bool any = false;
        for (u32 k = 0; k < r->count && !any; ++k) any = !strncmp(m[k].name, rows[row], strlen(rows[row])) && m[k].name[strlen(rows[row])] == ' ';
        if (!any) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(rows[row]);
        for (u32 c = 0; c < 4; ++c) {
            ImGui::TableNextColumn();
            snprintf(name, sizeof(name), "%s %s", rows[row], cols[c]);
            v[0] = 0;
            for (u32 k = 0; k < r->count; ++k)
                if (!strcmp(m[k].name, name)) {
                    char n[32];
                    format_value(n, sizeof(n), m[k].value);
                    snprintf(v, sizeof(v), "%s %s", n, m[k].unit);
                }
            theme_push_numbers();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(v).x);
            ImGui::TextUnformatted(v);
            ImGui::PopFont();
        }
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Bandwidth: all threads, each on its own block sized to fit the level. Latency: one thread, "
                        "random pointer chase. Copy counts bytes read + written.");
}

// Latency (y) against the bandwidth the other cores generate (x), one point per load level.
static void draw_loaded(const BenchResult* r) {
    const Metric* m = res_metrics(r);
    f32 bw[64], lat[64];
    u32 n = 0;
    for (u32 k = 0; k + 1 < r->count && n < 64; ++k)
        if (!strcmp(m[k].name, "Latency") && !strcmp(m[k + 1].name, "Bandwidth") && m[k].x == m[k + 1].x) {
            lat[n] = (f32)m[k].value;
            bw[n++] = (f32)m[k + 1].value;
        }
    if (!n) return;
    f32 bw_max = 1.0f, lat_max = 1.0f;
    for (u32 i = 0; i < n; ++i) {
        bw_max = bw[i] > bw_max ? bw[i] : bw_max;
        lat_max = lat[i] > lat_max ? lat[i] : lat_max;
    }
    bw_max *= 1.1f;
    lat_max *= 1.15f;
    ImGui::TextDisabled("Latency [ns] vs bandwidth the other cores generate [GB/s]");
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 12.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##loaded", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 pad = ImGui::GetFontSize() * 0.5f;
    const ImVec2 a(p0.x + pad * 5.0f, p0.y + pad), b(p0.x + size.x - pad, p0.y + size.y - pad * 3.0f);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(g_pal.container),
                      ImGui::GetStyle().FrameRounding);
    const ImU32 axis = ImGui::GetColorU32(g_pal.outline_variant), text = ImGui::GetColorU32(g_pal.on_surface_variant);
    char lbl[32];
    for (u32 i = 0; i <= 4; ++i) {      // grid lines + labels
        const f32 ty = b.y - (b.y - a.y) * i / 4.0f, tx = a.x + (b.x - a.x) * i / 4.0f;
        dl->AddLine(ImVec2(a.x, ty), ImVec2(b.x, ty), axis);
        snprintf(lbl, sizeof(lbl), "%.0f", lat_max * i / 4.0f);
        dl->AddText(ImVec2(p0.x + pad * 0.5f, ty - ImGui::GetFontSize() * 0.5f), text, lbl);
        snprintf(lbl, sizeof(lbl), "%.0f", bw_max * i / 4.0f);
        dl->AddText(ImVec2(tx - ImGui::CalcTextSize(lbl).x * 0.5f, b.y + pad * 0.3f), text, lbl);
    }
    const ImU32 line = ImGui::GetColorU32(g_pal.primary);
    ImVec2 pts[64];
    for (u32 i = 0; i < n; ++i) pts[i] = ImVec2(a.x + (b.x - a.x) * bw[i] / bw_max, b.y - (b.y - a.y) * lat[i] / lat_max);
    dl->AddPolyline(pts, (int)n, line, ImGui::GetFontSize() * 0.12f);
    i32 near = -1;
    f32 best = 1e30f;
    for (u32 i = 0; i < n; ++i) {
        dl->AddCircleFilled(pts[i], ImGui::GetFontSize() * 0.22f, line);
        const f32 dx = ImGui::GetIO().MousePos.x - pts[i].x, dy = ImGui::GetIO().MousePos.y - pts[i].y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; near = (i32)i; }
    }
    if (hovered && near >= 0)
        ImGui::SetTooltip("%.1f GB/s load: %.1f ns\n(delay %.0f cycles per 256 B per load thread)", bw[near], lat[near],
                          m[near * 2].x);
}

bool mem_chart_has_view(u32 bench) {
    const char* id = g_benches[bench].cli_id;
    return !strcmp(id, "mem.grid") || !strcmp(id, "mem.lat.loaded");
}

void mem_chart_draw(u32 bench, const BenchResult* r) {
    if (!strcmp(g_benches[bench].cli_id, "mem.grid")) draw_grid(r);
    else draw_loaded(r);
}
