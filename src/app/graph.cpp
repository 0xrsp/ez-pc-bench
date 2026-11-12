// Graph widget (see graph.h).
#include "app/graph.h"
#include "app/theme.h"
#include "core/results.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---- colors ---------------------------------------------------------------------------------------------
// One hue per kind of value, from the current theme (Palette::series, theme.cpp: e.g. Solarized's cyan / violet
// / yellow / red / magenta / blue / green / orange). Box, grid and border are neutral theme colors; only the
// line, its fill and the legend chips carry the kind's hue.

const char* const g_graph_kind_names[GK_COUNT] = {
    "Load / utilization", "Clock", "Power", "Temperature", "Memory / bandwidth", "Latency / time", "Disk / IOPS",
    "Compute throughput", "Count / other",
};
static_assert(GK_COUNT == THEME_SERIES, "one theme hue per graph kind");

// Extra lines of a multi-line graph take these hues in order, skipping the graph's own kind.
static const GraphKind COMPANIONS[] = { GK_POWER, GK_LOAD, GK_MEMORY, GK_DISK, GK_TEMP, GK_FREQ, GK_LATENCY, GK_RATE };

static const struct { const char* unit; GraphKind kind; } UNIT_KINDS[] = {
    { "%", GK_LOAD },
    { "MHz", GK_FREQ }, { "GHz", GK_FREQ }, { "kHz", GK_FREQ },
    { "W", GK_POWER }, { "mW", GK_POWER }, { "V", GK_POWER }, { "Wh", GK_POWER },
    { "\xC2\xB0" "C", GK_TEMP },
    { "GB/s", GK_MEMORY }, { "GiB/s", GK_MEMORY }, { "TB/s", GK_MEMORY },
    { "B", GK_MEMORY }, { "KiB", GK_MEMORY }, { "MiB", GK_MEMORY }, { "GiB", GK_MEMORY }, { "MB", GK_MEMORY },
    { "GB", GK_MEMORY },
    { "ns", GK_LATENCY }, { "\xC2\xB5s", GK_LATENCY }, { "us", GK_LATENCY }, { "ms", GK_LATENCY }, { "s", GK_LATENCY },
    { "clk", GK_LATENCY }, { "cc", GK_LATENCY }, { "cycles", GK_LATENCY },
    { "IOPS", GK_DISK }, { "MB/s", GK_DISK }, { "MiB/s", GK_DISK }, { "KB/s", GK_DISK },
    { "fps", GK_RATE },
    { "RPM", GK_OTHER }, { "x", GK_OTHER },
};

GraphKind graph_kind_for_unit(const char* unit) {
    if (!unit || !unit[0]) return GK_OTHER;
    for (u32 i = 0; i < ARRAY_COUNT(UNIT_KINDS); ++i)
        if (!strcmp(unit, UNIT_KINDS[i].unit)) return UNIT_KINDS[i].kind;
    const size_t n = strlen(unit);
    if (n > 2 && !strcmp(unit + n - 2, "/s")) return GK_RATE;      // Gops/s, Mtri/s, PSO/s, ...
    if (strstr(unit, "FLOPS") || strstr(unit, "OPS")) return GK_RATE;
    return GK_OTHER;
}

u32 graph_rgb(GraphKind k, bool dark) { return theme_series_rgb(dark ? THEME_DARK : THEME_LIGHT, k); }

static ImU32 rgb_u32(u32 rgb, f32 alpha) {
    const u32 a = (u32)(alpha * 255.0f + 0.5f);
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a > 255 ? 255 : a);
}

ImU32 graph_color(GraphKind k, f32 alpha) { return rgb_u32(g_pal.series[k < GK_COUNT ? k : GK_OTHER], alpha); }

static GraphKind line_kind(GraphKind k, u32 i) {
    if (i == 0) return k;
    u32 seen = 0;
    for (u32 j = 0; j < ARRAY_COUNT(COMPANIONS) * 2; ++j) {
        const GraphKind c = COMPANIONS[j % ARRAY_COUNT(COMPANIONS)];
        if (c == k) continue;
        if (++seen == i) return c;
    }
    return GK_OTHER;
}

ImU32 graph_line_color(GraphKind k, u32 i, f32 alpha) { return graph_color(line_kind(k, i), alpha); }

static GraphKind kind_of(const Graph* g, const GraphLine* l, u32 i) {
    return l->kind >= 0 && l->kind < GK_COUNT ? (GraphKind)l->kind : line_kind(g->kind, i);
}

// ---- text -----------------------------------------------------------------------------------------------

void graph_format(char* buf, u32 size, f64 v, const char* unit, i32 decimals) {
    if (!isfinite(v)) { snprintf(buf, size, "\xE2\x80\x94"); return; }
    if (!unit) unit = "";
    if (!strcmp(unit, "MHz") && fabs(v) >= 1000.0) { snprintf(buf, size, "%.2f GHz", v / 1000.0); return; }
    if (!strcmp(unit, "MiB") && fabs(v) >= 1024.0) { snprintf(buf, size, "%.1f GiB", v / 1024.0); return; }
    if (!strcmp(unit, "%")) { snprintf(buf, size, "%.*f%%", decimals < 0 ? 0 : decimals, v); return; }
    if (decimals < 0 && (!strcmp(unit, "MHz") || !strcmp(unit, "MiB") || !strcmp(unit, "RPM"))) decimals = 0;
    char n[32];
    if (decimals >= 0) snprintf(n, sizeof(n), "%.*f", decimals, v);
    else format_value(n, sizeof(n), v);
    snprintf(buf, size, unit[0] ? "%s %s" : "%s", n, unit);
}

// Compact axis / caption text: "100%", "5 GHz", "2.5 W".
static void axis_text(char* buf, u32 size, f64 v, const char* unit) {
    if (!unit) unit = "";
    if (!strcmp(unit, "MHz") && fabs(v) >= 1000.0) { snprintf(buf, size, "%g GHz", v / 1000.0); return; }
    if (!strcmp(unit, "MiB") && fabs(v) >= 1024.0) { snprintf(buf, size, "%g GiB", v / 1024.0); return; }
    if (!strcmp(unit, "%")) { snprintf(buf, size, "%g%%", v); return; }
    char n[32];
    if (fabs(v) >= 100000.0) snprintf(n, sizeof(n), "%.0f", v);
    else snprintf(n, sizeof(n), "%g", (f64)(f32)v);
    snprintf(buf, size, unit[0] ? "%s %s" : "%s", n, unit);
}

static ImU32 text_col() { return ImGui::GetColorU32(ImGuiCol_Text); }
static ImU32 dim_col() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
static void push_small() { ImGui::PushFont(nullptr, theme_font_size(0.85f)); }

// ---- range ----------------------------------------------------------------------------------------------

constexpr u32 GRID_COLS = 10, GRID_ROWS = 5;

static f64 nice_step(f64 raw) {     // 1, 2, 2.5 or 5 x 10^k, >= raw
    if (!(raw > 0.0)) return 1.0;
    const f64 p = pow(10.0, floor(log10(raw)));
    const f64 m = raw / p;
    return (m <= 1.0 ? 1.0 : m <= 2.0 ? 2.0 : m <= 2.5 ? 2.5 : m <= 5.0 ? 5.0 : 10.0) * p;
}

// Range from the data (dmin..dmax, NaN = none) and the graph's fixed ends; free ends snap to a nice step.
static void make_range(const Graph* g, f32 dmin, f32 dmax, f32* lo, f32* hi) {
    const bool fix_lo = !isnan(g->lo), fix_hi = !isnan(g->hi);
    if (fix_lo && fix_hi) { *lo = g->lo; *hi = g->hi > g->lo ? g->hi : g->lo + 1.0f; return; }
    if (isnan(dmin)) { dmin = fix_lo ? g->lo : 0.0f; dmax = fix_hi ? g->hi : dmin + 1.0f; }
    f64 a = fix_lo ? g->lo : dmin, b = fix_hi ? g->hi : dmax;
    if (!fix_lo && (g->flags & GF_ZERO) && a > 0.0) a = 0.0;
    if (!fix_hi && (g->flags & GF_ZERO) && b < 0.0) b = 0.0;
    f64 span = b - a;
    const f64 min_span = g->min_span > 0.0f ? g->min_span : fabs(b) > 0.0 ? fabs(b) * 0.1 : 1.0;
    if (span < min_span) {      // grow around the middle, or away from a fixed end
        const f64 grow = min_span - span;
        if (fix_lo || ((g->flags & GF_ZERO) && a == 0.0)) b += grow;
        else if (fix_hi) a -= grow;
        else { a -= grow * 0.5; b += grow * 0.5; }
        span = b - a;
    }
    const bool zero_based = (g->flags & GF_ZERO) != 0;
    if (!zero_based) {          // breathing room above and below the line
        if (!fix_lo) a -= span * 0.08;
        if (!fix_hi) b += span * 0.08;
    }
    const f64 step = nice_step((b - a) / GRID_ROWS);
    if (!fix_lo) a = floor(a / step) * step;
    if (!fix_hi) b = ceil(b / step) * step;
    if (b <= a) b = a + step;
    *lo = (f32)a;
    *hi = (f32)b;
}

// ---- drawing pieces -------------------------------------------------------------------------------------

static f32 line_thickness() {
    const f32 t = ImGui::GetFontSize() * 0.085f;
    return t < 1.0f ? 1.0f : t;
}

// Background, grid and border of the box a..b (pixel aligned), in neutral theme colors.
static void draw_box(ImDrawList* dl, ImVec2 a, ImVec2 b, bool grid, const f32* vx, u32 vx_count) {
    const ImVec4 t = g_pal.on_surface;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(t.x, t.y, t.z, theme_is_dark() ? 0.025f : 0.02f)));
    if (grid && b.y - a.y >= ImGui::GetFontSize() * 1.8f) {      // no grid in table-row sized graphs
        const ImU32 gc = ImGui::GetColorU32(g_pal.outline_variant);
        const f32 w = b.x - a.x, h = b.y - a.y;
        if (vx) {
            for (u32 i = 0; i < vx_count; ++i) dl->AddLine(ImVec2(vx[i] + 0.5f, a.y), ImVec2(vx[i] + 0.5f, b.y), gc);
        } else if (w / GRID_COLS >= 6.0f) {
            for (u32 i = 1; i < GRID_COLS; ++i) {
                const f32 x = floorf(a.x + w * i / GRID_COLS) + 0.5f;
                dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), gc);
            }
        }
        const u32 rows = h / GRID_ROWS >= 6.0f ? GRID_ROWS : h / 2.0f >= 6.0f ? 2 : 1;
        for (u32 i = 1; i < rows; ++i) {
            const f32 y = floorf(a.y + h * i / rows) + 0.5f;
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), gc);
        }
    }
    dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border));     // the frames' border (outline_variant .. outline)
}

// Flat translucent area between the polyline and `base`.
static void fill_under(ImDrawList* dl, const ImVec2* pts, u32 n, f32 base, ImU32 fill) {
    if (n < 2) return;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve((int)((n - 1) * 6), (int)(n * 2));
    const u32 v0 = dl->_VtxCurrentIdx;
    for (u32 i = 0; i < n; ++i) {
        dl->PrimWriteVtx(pts[i], uv, fill);
        dl->PrimWriteVtx(ImVec2(pts[i].x, base), uv, fill);
    }
    for (u32 i = 0; i + 1 < n; ++i) {
        const ImDrawIdx a = (ImDrawIdx)(v0 + i * 2);
        dl->PrimWriteIdx(a); dl->PrimWriteIdx((ImDrawIdx)(a + 1)); dl->PrimWriteIdx((ImDrawIdx)(a + 3));
        dl->PrimWriteIdx(a); dl->PrimWriteIdx((ImDrawIdx)(a + 3)); dl->PrimWriteIdx((ImDrawIdx)(a + 2));
    }
}

// Line plus fill for points already in screen space; NaN y (gap) splits the line into runs.
static void draw_runs(ImDrawList* dl, const ImVec2* pts, u32 n, f32 base, GraphKind k, bool fill) {
    const ImU32 col = graph_color(k), area = graph_color(k, theme_is_dark() ? 0.20f : 0.16f);
    const f32 th = line_thickness();
    for (u32 i = 0; i < n;) {
        while (i < n && isnan(pts[i].y)) ++i;
        u32 j = i;
        while (j < n && !isnan(pts[j].y)) ++j;
        if (j - i == 1) dl->AddCircleFilled(pts[i], th * 1.2f, col);
        else if (j > i) {
            if (fill) fill_under(dl, pts + i, j - i, base, area);
            dl->AddPolyline(pts + i, (int)(j - i), col, th);
        }
        i = j;
    }
}

struct Frame { ImVec2 p, a, b; bool hovered, spark; f32 small_h; };

// Reserves the item and lays out caption and box.
static Frame begin_frame(const char* id, const Graph* g, ImVec2 size, f32 left_margin, f32 bottom_margin) {
    Frame f;
    f.spark = (g->flags & GF_SPARK) != 0;
    const f32 w = size.x > 0.0f ? size.x : ImGui::GetContentRegionAvail().x;
    const f32 h = size.y > 4.0f ? size.y : 4.0f;
    f.p = ImGui::GetCursorScreenPos();
    if (f.spark) {
        ImGui::Dummy(ImVec2(w, h));
        f.hovered = false;
    } else {
        ImGui::InvisibleButton(id, ImVec2(w > 4.0f ? w : 4.0f, h));
        f.hovered = ImGui::IsItemHovered();
    }
    push_small();
    f.small_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const bool caption = !f.spark && !(g->flags & GF_NO_CAPTION);
    const f32 top = caption ? f.small_h + 3.0f : 0.0f;
    f.a = ImVec2(floorf(f.p.x + left_margin), floorf(f.p.y + top));
    f.b = ImVec2(floorf(f.p.x + w), floorf(f.p.y + h - bottom_margin));
    if (f.b.y < f.a.y + 4.0f) f.b.y = f.a.y + 4.0f;
    return f;
}

// Caption: title (and a legend for several named lines) left, `right` right-aligned.
static void draw_caption(ImDrawList* dl, const Frame* f, const Graph* g, const GraphLine* lines, u32 count,
                         const char* right) {
    if (f->spark || (g->flags & GF_NO_CAPTION)) return;
    push_small();
    const f32 y = f->p.y;
    f32 x = f->a.x, right_x = f->b.x;
    if (right && right[0]) {
        right_x = f->b.x - ImGui::CalcTextSize(right).x;
        dl->AddText(ImVec2(right_x, y), g->right_col ? g->right_col : dim_col(), right);
    }
    dl->PushClipRect(ImVec2(f->p.x, y), ImVec2(right_x - ImGui::GetFontSize() * 0.5f, y + f->small_h), true);
    if (g->title) {
        dl->AddText(ImVec2(x, y), dim_col(), g->title);
        x += ImGui::CalcTextSize(g->title).x + ImGui::GetFontSize();
    }
    if (count > 1) {
        const f32 sq = ImGui::GetFontSize() * 0.55f;
        for (u32 i = 0; i < count; ++i) {
            if (!lines[i].name) continue;
            const f32 sy = y + (f->small_h - sq) * 0.5f;
            dl->AddRectFilled(ImVec2(x, sy), ImVec2(x + sq, sy + sq), graph_color(kind_of(g, &lines[i], i)), 2.0f);
            x += sq + 4.0f;
            dl->AddText(ImVec2(x, y), text_col(), lines[i].name);
            x += ImGui::CalcTextSize(lines[i].name).x + ImGui::GetFontSize() * 0.9f;
        }
    }
    dl->PopClipRect();
    ImGui::PopFont();
}

static void value_text(char* buf, u32 size, const Graph* g, f64 v) {
    if (g->fmt_y) g->fmt_y(buf, size, v, g->user);
    else graph_format(buf, size, v, g->unit, g->decimals);
}

// ---- time series ----------------------------------------------------------------------------------------

constexpr u32 MAX_LINES = 8, MAX_POINTS = 2048;
static ImVec2 s_pts[MAX_POINTS];

GraphHit graph_time(const char* id, const Graph* g, const GraphLine* lines, u32 count, ImVec2 size) {
    GraphHit hit;
    count = count < MAX_LINES ? count : MAX_LINES;
    u32 window = g->window;
    for (u32 l = 0; l < count && !g->window; ++l) window = lines[l].n > window ? lines[l].n : window;
    if (window < 2) window = 2;
    window = window < MAX_POINTS ? window : MAX_POINTS;

    f32 dmin = NAN, dmax = NAN;
    for (u32 l = 0; l < count; ++l) {
        const GraphLine* ln = &lines[l];
        const u32 first = ln->n > window ? ln->n - window : 0;
        for (u32 i = first; i < ln->n; ++i) {
            const f32 v = ln->y[i];
            if (isnan(v)) continue;
            dmin = isnan(dmin) || v < dmin ? v : dmin;
            dmax = isnan(dmax) || v > dmax ? v : dmax;
        }
    }
    f32 lo, hi;
    make_range(g, dmin, dmax, &lo, &hi);

    const Frame f = begin_frame(id, g, size, 0.0f, 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    char right[48];
    if (g->right) snprintf(right, sizeof(right), "%s", g->right);
    else axis_text(right, sizeof(right), hi, g->unit);
    draw_caption(dl, &f, g, lines, count, right);
    draw_box(dl, f.a, f.b, !f.spark, nullptr, 0);

    const f32 inset = 1.0f;
    const f32 left = f.a.x + inset, rx = f.b.x - inset, top = f.a.y + inset, bottom = f.b.y - inset;
    const f32 dx = (rx - left) / (f32)(window - 1);
    auto ypos = [&](f32 v) {
        const f32 t = (v - lo) / (hi - lo);
        return bottom - (bottom - top) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
    };

    // More samples than the box has room for (long windows, thumbnails): one point per `b` samples, their average,
    // so the line doesn't turn into a dense zigzag. Buckets are aligned to the absolute sample index (`g->end`), so
    // they stay put as new samples come in.
    u32 longest = 0;
    for (u32 l = 0; l < count; ++l) longest = lines[l].n > longest ? lines[l].n : longest;
    const u32 room = (u32)((rx - left) / (ImGui::GetFontSize() * 0.2f));
    const u32 b = room >= 2 && window > room ? (window + room - 1) / room : 1;
    const u32 end = g->end >= longest ? g->end : longest;     // samples ever recorded; the newest is end - 1
    // Samples `back0` .. `back1` before the newest (inclusive) that share a bucket with sample `back`.
    auto bucket = [&](u32 back, u32* back0, u32* back1) {
        const u32 abs = end - 1 - back, start = abs - abs % b;     // bucket = samples start .. start + b - 1
        *back0 = start + b - 1 >= end - 1 ? 0 : end - 1 - (start + b - 1);
        *back1 = end - 1 - start;
    };
    // Average of line `ln` over samples back0..back1 (NaN = no value there); `t` = their mean age in samples.
    auto average = [&](const GraphLine* ln, u32 back0, u32 back1, f32* t) {
        f32 sum = 0.0f, tsum = 0.0f;
        u32 c = 0;
        for (u32 k = back0; k <= back1 && k < ln->n && k < window; ++k) {
            const f32 v = ln->y[ln->n - 1 - k];
            if (isnan(v)) continue;
            sum += v;
            tsum += (f32)k;
            ++c;
        }
        if (t) *t = c ? tsum / (f32)c : 0.5f * (f32)(back0 + back1);
        return c ? sum / (f32)c : NAN;
    };

    dl->PushClipRect(f.a, ImVec2(f.b.x + 1.0f, f.b.y + 1.0f), true);
    for (u32 l = count; l-- > 0;) {     // line 0 on top
        const GraphLine* ln = &lines[l];
        const u32 n = ln->n < window ? ln->n : window;
        u32 m = 0;
        for (u32 back = n; back-- > 0 && m < MAX_POINTS;) {    // oldest first
            u32 back0, back1;
            bucket(back, &back0, &back1);
            f32 t;
            const f32 v = average(ln, back0, back1, &t);
            s_pts[m++] = ImVec2(rx - dx * t, isnan(v) ? NAN : ypos(v));
            back = back0;
        }
        draw_runs(dl, s_pts, m, bottom, kind_of(g, ln, l), !(g->flags & GF_NO_FILL) && count == 1);
    }

    // Hover: the sample (or bucket) under the mouse on every line.
    if (f.hovered && count) {
        const f32 mx = ImGui::GetIO().MousePos.x;
        const i32 back = (i32)floorf((rx - mx) / dx + 0.5f);     // samples before the newest
        if (back >= 0 && back < (i32)window && back < (i32)longest) {
            u32 back0, back1;
            bucket((u32)back, &back0, &back1);
            f32 vals[MAX_LINES], t = NAN;
            for (u32 l = 0; l < count; ++l) {
                f32 tl;
                vals[l] = average(&lines[l], back0, back1, &tl);
                if (isnan(t) && !isnan(vals[l])) t = tl;
            }
            if (b == 1 || isnan(t)) t = (f32)back;
            const f32 x = rx - dx * t;
            dl->AddLine(ImVec2(x, f.a.y), ImVec2(x, f.b.y), dim_col(), 1.0f);
            for (u32 l = 0; l < count; ++l) {
                if (isnan(vals[l])) continue;
                if (hit.line < 0) {
                    hit.line = (i32)l;
                    hit.point = (i32)lines[l].n - 1 - (i32)back0;
                    hit.value = vals[l];
                }
                dl->AddCircleFilled(ImVec2(x, ypos(vals[l])), line_thickness() * 2.5f, graph_color(kind_of(g, &lines[l], l)));
            }
            if (!(g->flags & GF_NO_TOOLTIP)) {
                ImGui::BeginTooltip();
                char v[48];
                bool any = false;
                for (u32 l = 0; l < count; ++l) {
                    if ((i32)lines[l].n - 1 - (i32)back0 < 0) continue;
                    value_text(v, sizeof(v), g, vals[l]);
                    const char* name = lines[l].name ? lines[l].name : g->title;
                    if (name) ImGui::Text("%s: %s", name, v);
                    else ImGui::TextUnformatted(v);
                    any = true;
                }
                if (!any) ImGui::TextDisabled("No data yet");
                if (g->step_s > 0.0f) {
                    char ago[48];
                    const f32 s = g->step_s * (f32)back0;
                    if (back0 == 0) snprintf(ago, sizeof(ago), "Now");
                    else if (s < 120.0f) snprintf(ago, sizeof(ago), "%.0f s ago", s);
                    else snprintf(ago, sizeof(ago), "%u min %02u s ago", (u32)s / 60, (u32)s % 60);
                    if (b > 1) ImGui::TextDisabled("%s, average of %.0f s", ago, g->step_s * (f32)(back1 - back0 + 1));
                    else ImGui::TextDisabled("%s", ago);
                }
                ImGui::EndTooltip();
            }
        }
    }

    if ((g->flags & GF_VALUE) && count && lines[0].n) {
        f32 last = NAN;
        for (u32 i = lines[0].n; i-- > 0 && isnan(last);) last = lines[0].y[i];
        char v[48];
        value_text(v, sizeof(v), g, last);
        const f32 pad = ImGui::GetFontSize() * 0.3f;
        dl->AddText(ImVec2(f.a.x + pad, f.a.y + pad * 0.5f), text_col(), v);
    }
    dl->PopClipRect();
    return hit;
}

// ---- x / y ----------------------------------------------------------------------------------------------

static void x_text(char* buf, u32 size, const Graph* g, f64 x) {
    if (g->fmt_x) g->fmt_x(buf, size, x, g->user);
    else if (g->x_unit) format_x(buf, size, x, g->x_unit);
    else snprintf(buf, size, "%g", x);
}

static f64 line_x(const GraphLine* l, u32 i) { return l->x ? (f64)l->x[i] : (f64)i; }

GraphHit graph_xy(const char* id, const Graph* g, const GraphLine* lines, u32 count, ImVec2 size) {
    GraphHit hit;
    count = count < MAX_LINES ? count : MAX_LINES;
    const bool logx = (g->flags & GF_LOG_X) != 0;
    f32 dmin = NAN, dmax = NAN;
    f64 xmin = INFINITY, xmax = -INFINITY;
    for (u32 l = 0; l < count; ++l)
        for (u32 i = 0; i < lines[l].n; ++i) {
            const f32 v = lines[l].y[i];
            const f64 x = line_x(&lines[l], i);
            if (isnan(v) || (logx && !(x > 0.0))) continue;
            dmin = isnan(dmin) || v < dmin ? v : dmin;
            dmax = isnan(dmax) || v > dmax ? v : dmax;
            xmin = x < xmin ? x : xmin;
            xmax = x > xmax ? x : xmax;
        }
    f32 lo, hi;
    make_range(g, dmin, dmax, &lo, &hi);
    const bool spark = (g->flags & GF_SPARK) != 0;

    // Margins for the tick labels: y labels left, x labels below.
    char t[64];
    push_small();
    const f32 sfs = ImGui::GetFontSize(), small_h = ImGui::GetTextLineHeight();
    f32 label_w = 0.0f;
    if (!spark)
        for (u32 r = 0; r <= GRID_ROWS; ++r) {
            const f64 v = lo + (hi - lo) * r / GRID_ROWS;
            if (g->fmt_y) g->fmt_y(t, sizeof(t), v, g->user);
            else axis_text(t, sizeof(t), v, "");
            const f32 w = ImGui::CalcTextSize(t).x;
            label_w = w > label_w ? w : label_w;
        }
    ImGui::PopFont();
    const f32 left_margin = spark ? 0.0f : label_w + sfs * 0.6f, bottom_margin = spark ? 0.0f : small_h + 4.0f;
    const Frame f = begin_frame(id, g, size, left_margin, bottom_margin);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_caption(dl, &f, g, lines, count, g->right);

    const f32 inset = sfs * 0.5f;
    const f32 left = f.a.x + inset, rx = f.b.x - inset, top = f.a.y + inset * 0.5f, bottom = f.b.y - 1.0f;
    const bool one_x = !(xmax > xmin);
    auto xpos = [&](f64 x) -> f32 {
        if (one_x) return (left + rx) * 0.5f;
        const f64 t = logx ? (log2(x) - log2(xmin)) / (log2(xmax) - log2(xmin)) : (x - xmin) / (xmax - xmin);
        return left + (rx - left) * (f32)t;
    };
    auto ypos = [&](f32 v) {
        const f32 t = (v - lo) / (hi - lo);
        return bottom - (bottom - top) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
    };

    // x ticks at the data points of the longest line, skipping labels that would overlap.
    static f32 tick_x[64];
    u32 ticks = 0;
    u32 tl = 0;
    for (u32 l = 1; l < count; ++l) tl = lines[l].n > lines[tl].n ? l : tl;
    push_small();
    if (!spark && count && isfinite(xmin)) {
        f32 last_right = -INFINITY;
        const GraphLine* ln = &lines[tl];
        for (u32 i = 0; i < ln->n && ticks < ARRAY_COUNT(tick_x); ++i) {
            const f64 x = line_x(ln, i);
            if (logx && !(x > 0.0)) continue;
            x_text(t, sizeof(t), g, x);
            const f32 px = xpos(x), w = ImGui::CalcTextSize(t).x;
            f32 lx = px - w * 0.5f;
            if (lx < f.a.x) lx = f.a.x;
            if (lx + w > f.b.x) lx = f.b.x - w;
            if (lx < last_right + sfs * 0.8f) continue;
            dl->AddText(ImVec2(lx, f.b.y + 3.0f), dim_col(), t);
            last_right = lx + w;
            tick_x[ticks++] = floorf(px);
        }
        for (u32 r = 0; r <= GRID_ROWS; ++r) {      // y labels on the grid rows
            const f64 v = lo + (hi - lo) * r / GRID_ROWS;
            if (g->fmt_y) g->fmt_y(t, sizeof(t), v, g->user);
            else axis_text(t, sizeof(t), v, "");
            const f32 y = f.b.y - (f.b.y - f.a.y) * r / GRID_ROWS;
            f32 ty = y - small_h * 0.5f;
            if (ty < f.a.y - 2.0f) ty = f.a.y - 2.0f;
            if (ty + small_h > f.b.y + 2.0f) ty = f.b.y + 2.0f - small_h;
            dl->AddText(ImVec2(f.a.x - sfs * 0.3f - ImGui::CalcTextSize(t).x, ty), dim_col(), t);
        }
    }
    ImGui::PopFont();
    draw_box(dl, f.a, f.b, !spark, ticks ? tick_x : nullptr, ticks);

    // Lines (NaN and non-positive x on a log axis = gap) and points.
    dl->PushClipRect(f.a, ImVec2(f.b.x + 1.0f, f.b.y + 1.0f), true);
    const f32 dot = sfs * 0.17f;
    for (u32 l = count; l-- > 0;) {
        const GraphLine* ln = &lines[l];
        const u32 n = ln->n < MAX_POINTS ? ln->n : MAX_POINTS;
        for (u32 i = 0; i < n; ++i) {
            const f64 x = line_x(ln, i);
            const bool ok = !isnan(ln->y[i]) && (!logx || x > 0.0);
            s_pts[i] = ImVec2(ok ? xpos(x) : 0.0f, ok ? ypos(ln->y[i]) : NAN);
        }
        const GraphKind k = kind_of(g, ln, l);
        draw_runs(dl, s_pts, n, bottom, k, count == 1 && !(g->flags & GF_NO_FILL));
        if (!(g->flags & GF_POINTS) && !ln->marked) continue;
        for (u32 i = 0; i < n; ++i) {
            if (isnan(s_pts[i].y)) continue;
            const bool mark = ln->marked && ln->marked[i];
            if (mark) dl->AddCircleFilled(s_pts[i], dot * 1.5f, g->mark_col ? g->mark_col : graph_color(GK_POWER));
            else if (g->flags & GF_POINTS) dl->AddCircleFilled(s_pts[i], dot, graph_color(k));
        }
    }
    dl->PopClipRect();

    // Hover: nearest point in x (ties: nearest in y) over all lines.
    if (f.hovered && count) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        f32 best = INFINITY;
        for (u32 l = 0; l < count; ++l)
            for (u32 i = 0; i < lines[l].n; ++i) {
                const f64 x = line_x(&lines[l], i);
                if (isnan(lines[l].y[i]) || (logx && !(x > 0.0))) continue;
                const f32 d = fabsf(xpos(x) - m.x) * 4.0f + fabsf(ypos(lines[l].y[i]) - m.y);
                if (d < best) { best = d; hit.line = (i32)l; hit.point = (i32)i; }
            }
        if (hit.line >= 0) {
            const f64 hx = line_x(&lines[hit.line], (u32)hit.point);
            const f32 px = xpos(hx);
            dl->AddLine(ImVec2(px, f.a.y), ImVec2(px, f.b.y), dim_col(), 1.0f);
            for (u32 l = 0; l < count; ++l)
                for (u32 i = 0; i < lines[l].n; ++i)
                    if (line_x(&lines[l], i) == hx && !isnan(lines[l].y[i])) {
                        const ImVec2 c(px, ypos(lines[l].y[i]));
                        const ImU32 col = graph_color(kind_of(g, &lines[l], l));
                        dl->AddCircleFilled(c, dot * 1.6f, col);
                        dl->AddCircle(c, dot * 2.8f, col, 0, 1.5f);
                    }
            if (!(g->flags & GF_NO_TOOLTIP)) {
                ImGui::BeginTooltip();
                char x[64], v[48];
                x_text(x, sizeof(x), g, hx);
                ImGui::TextDisabled("%s", x);
                for (u32 l = 0; l < count; ++l)
                    for (u32 i = 0; i < lines[l].n; ++i) {
                        if (line_x(&lines[l], i) != hx || isnan(lines[l].y[i])) continue;
                        value_text(v, sizeof(v), g, lines[l].y[i]);
                        if (lines[l].name) ImGui::Text("%s: %s", lines[l].name, v);
                        else ImGui::TextUnformatted(v);
                    }
                ImGui::EndTooltip();
            }
        }
    }
    return hit;
}

// ---- bar and stat ---------------------------------------------------------------------------------------

void graph_bar(f32 frac, f32 marker, ImVec2 size, ImU32 col) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 line_h = ImGui::GetTextLineHeight();
    const f32 y = floorf(p.y + (line_h - size.y) * 0.5f), x = floorf(p.x);
    const ImVec2 a(x, y), b(floorf(x + size.x), floorf(y + size.y));
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
    auto with_alpha = [&](f32 alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); };
    const f32 r = 2.0f;
    dl->AddRectFilled(a, b, with_alpha(0.10f), r);
    const f32 fw = (b.x - a.x) * (frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac);
    if (fw >= 1.0f) {
        const ImVec2 fb(floorf(a.x + fw), b.y);
        dl->AddRectFilledMultiColor(a, fb, with_alpha(0.30f), with_alpha(0.60f), with_alpha(0.60f), with_alpha(0.30f));
        dl->AddRect(a, fb, with_alpha(0.9f), r);
    }
    dl->AddRect(a, b, with_alpha(0.35f), r);
    if (marker > 0.0f && marker < 1.0f) {
        const f32 mx = floorf(a.x + (b.x - a.x) * marker) + 0.5f;
        dl->AddLine(ImVec2(mx, a.y - 2.0f), ImVec2(mx, b.y + 2.0f), dim_col(), 1.5f);
    }
    ImGui::Dummy(ImVec2(size.x, line_h > size.y ? line_h : size.y));
}

void graph_stat(const char* label, const char* value, i32 kind) {
    ImGui::BeginGroup();
    push_small();
    if (kind >= 0 && kind < GK_COUNT) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const f32 d = ImGui::GetFontSize() * 0.5f, h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + (h - d) * 0.5f), ImVec2(p.x + d, p.y + (h + d) * 0.5f),
                                                  graph_color((GraphKind)kind), 2.0f);
        ImGui::Dummy(ImVec2(d, h));
        ImGui::SameLine(0.0f, d * 0.6f);
    }
    ImGui::TextDisabled("%s", label);
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.45f));
    ImGui::TextUnformatted(value);
    ImGui::PopFont();
    ImGui::EndGroup();
}
