// Frame-time graph shared by the frame capture and compare windows (see frames_graph.h).
#include "app/frames_graph.h"
#include "app/theme.h"
#include <math.h>
#include <stdio.h>

ImU32 frame_graph_color(u32 i) {
    static const ImU32 colors[] = {
        IM_COL32(26, 115, 232, 255), IM_COL32(217, 48, 37, 255), IM_COL32(24, 128, 56, 255), IM_COL32(227, 116, 0, 255),
        IM_COL32(147, 52, 230, 255), IM_COL32(18, 164, 175, 255), IM_COL32(176, 96, 0, 255), IM_COL32(95, 99, 104, 255),
    };
    return colors[i % ARRAY_COUNT(colors)];
}

// Smallest "nice" step (1, 2, 5 x 10^k) that splits `range` into at most `max_lines` parts.
static f32 nice_step(f32 range, f32 max_lines) {
    static const f32 mult[] = { 1.0f, 2.0f, 5.0f };
    f32 base = powf(10.0f, floorf(log10f(range > 0.0f ? range : 1.0f)) - 1.0f);
    for (;;) {
        for (f32 m : mult)
            if (range / (base * m) <= max_lines) return base * m;
        base *= 10.0f;
    }
}

constexpr u32 MAX_COLS = 4096;

void frame_graph(const char* id, const FrameSeries* s, u32 n, f32 y_max_ms, f32 height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 fs = ImGui::GetFontSize();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const f32 w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton(id, ImVec2(w, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 grid = ImGui::GetColorU32(g_pal.outline_variant), text = ImGui::GetColorU32(g_pal.on_surface_variant);
    if (y_max_ms <= 0.0f) y_max_ms = 20.0f;

    f32 t_max = 0.0f;
    for (u32 k = 0; k < n; ++k) {
        f64 t = 0.0;
        for (u32 i = 0; i < s[k].count; ++i) t += s[k].ms[i];
        t_max = (f32)(t / 1000.0) > t_max ? (f32)(t / 1000.0) : t_max;
    }
    if (t_max <= 0.0f) t_max = 1.0f;

    const f32 left = ImGui::CalcTextSize("000 ms").x + fs * 0.6f, bottom = fs * 1.4f;
    const ImVec2 a(p0.x + left, p0.y + fs * 0.5f), b(p0.x + w - fs * 0.5f, p0.y + height - bottom);
    const f32 pw = b.x - a.x, ph = b.y - a.y;
    if (pw < 16.0f || ph < 16.0f) return;
    char label[32];

    const f32 ys = nice_step(y_max_ms, 5.0f);
    for (f32 v = 0.0f; v <= y_max_ms + 1e-3f; v += ys) {
        const f32 y = b.y - v / y_max_ms * ph;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), grid);
        snprintf(label, sizeof(label), "%g ms", v);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(a.x - ts.x - fs * 0.3f, y - ts.y * 0.5f), text, label);
    }
    const f32 xs = nice_step(t_max, 8.0f);
    for (f32 t = 0.0f; t <= t_max + 1e-3f; t += xs) {
        const f32 x = a.x + t / t_max * pw;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), grid);
        snprintf(label, sizeof(label), "%g s", t);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(x - ts.x * 0.5f, b.y + fs * 0.2f), text, label);
    }

    // Slowest frame per pixel column, as a polyline.
    const u32 cols = (u32)pw < MAX_COLS ? (u32)pw : MAX_COLS;
    static f32 colmax[8][MAX_COLS];
    static ImVec2 pts[MAX_COLS];
    const u32 ns = n < 8 ? n : 8;
    for (u32 k = 0; k < ns; ++k) {
        f32* cm = colmax[k];
        for (u32 c = 0; c < cols; ++c) cm[c] = -1.0f;
        f64 t = 0.0;
        for (u32 i = 0; i < s[k].count; ++i) {
            t += s[k].ms[i] / 1000.0;
            u32 c = (u32)(t / t_max * cols);
            if (c >= cols) c = cols - 1;
            if (s[k].ms[i] > cm[c]) cm[c] = s[k].ms[i];
        }
        u32 np = 0;
        for (u32 c = 0; c < cols; ++c) {
            if (cm[c] < 0.0f) continue;
            const f32 v = cm[c] < y_max_ms ? cm[c] : y_max_ms;
            pts[np++] = ImVec2(a.x + (c + 0.5f) * pw / cols, b.y - v / y_max_ms * ph);
        }
        dl->PushClipRect(a, ImVec2(b.x + 1.0f, b.y + 1.0f), true);
        if (np > 1) dl->AddPolyline(pts, (int)np, frame_graph_color(k), fs * 0.09f);
        dl->PopClipRect();
    }

    if (hovered) {
        const f32 mx = ImGui::GetIO().MousePos.x;
        if (mx >= a.x && mx <= b.x) {
            dl->AddLine(ImVec2(mx, a.y), ImVec2(mx, b.y), text);
            const u32 c = (u32)((mx - a.x) / pw * cols) < cols ? (u32)((mx - a.x) / pw * cols) : cols - 1;
            ImGui::BeginTooltip();
            ImGui::Text("%.2f s", (mx - a.x) / pw * t_max);
            for (u32 k = 0; k < ns; ++k) {
                if (colmax[k][c] < 0.0f) continue;
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(frame_graph_color(k)), "%s: %.2f ms (%.0f fps)", s[k].label,
                                   colmax[k][c], colmax[k][c] > 0.0f ? 1000.0f / colmax[k][c] : 0.0f);
            }
            ImGui::EndTooltip();
        }
    }
}
