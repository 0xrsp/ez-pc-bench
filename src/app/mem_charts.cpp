// Memory result views for the Charts tab (see mem_charts.h).
#include "app/mem_charts.h"
#include "app/graph.h"
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
    f32 bw[64], lat[64], delay[64];
    u32 n = 0;
    for (u32 k = 0; k + 1 < r->count && n < 64; ++k)
        if (!strcmp(m[k].name, "Latency") && !strcmp(m[k + 1].name, "Bandwidth") && m[k].x == m[k + 1].x) {
            lat[n] = (f32)m[k].value;
            delay[n] = (f32)m[k].x;
            bw[n++] = (f32)m[k + 1].value;
        }
    if (!n) return;
    bool rising = true;     // bandwidth grows with the load: the area under the curve reads well
    for (u32 i = 1; i < n; ++i) rising &= bw[i] >= bw[i - 1];
    Graph g;
    g.title = "Latency [ns] vs bandwidth the other cores generate [GB/s]";
    g.unit = "ns";
    g.kind = GK_LATENCY;
    g.flags = GF_ZERO | GF_POINTS | GF_NO_TOOLTIP | (rising ? 0u : GF_NO_FILL);
    g.x_unit = "GB/s";
    g.fmt_x = [](char* buf, u32 size, f64 x, const void*) { snprintf(buf, size, x < 10.0 ? "%.1f GB/s" : "%.0f GB/s", x); };
    GraphLine line;
    line.x = bw;
    line.y = lat;
    line.n = n;
    const GraphHit hit = graph_xy("##loaded", &g, &line, 1, ImVec2(0.0f, ImGui::GetFontSize() * 12.0f));
    if (hit.point >= 0)
        ImGui::SetTooltip("%.1f GB/s load: %.1f ns\n(delay %.0f cycles per 256 B per load thread)", bw[hit.point],
                          lat[hit.point], delay[hit.point]);
}

bool mem_chart_has_view(u32 bench) {
    const char* id = g_benches[bench].cli_id;
    return !strcmp(id, "mem.grid") || !strcmp(id, "mem.lat.loaded");
}

void mem_chart_draw(u32 bench, const BenchResult* r) {
    if (!strcmp(g_benches[bench].cli_id, "mem.grid")) draw_grid(r);
    else draw_loaded(r);
}
