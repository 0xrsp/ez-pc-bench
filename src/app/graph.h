#pragma once
// Shared graph widget in the style of Windows Task Manager's Performance page, drawn with ImDrawList:
// a box with a neutral 1 px border and a faint 10 x 5 grid (theme colors), the line in the series color over a
// translucent fill of the same hue, a small caption above (title left, range max right). Two kinds:
//   graph_time   time series, newest sample at the right, a fixed window of samples across the width
//   graph_xy     x / y series for benchmark charts (optional log x), tick labels, hover tooltip per point
// Line colors come from the kind of value (load, clock, power, ...); units map to kinds with
// graph_kind_for_unit so benchmark charts are colored automatically. Each theme has its own hue per kind
// (Palette::series, theme.cpp). Also: graph_bar (horizontal bar) and graph_stat (Task Manager's big number
// with a small label).
#include "core/types.h"
#include "imgui.h"
#include <math.h>

enum GraphKind : u8 {
    GK_LOAD,        // load / utilization %
    GK_FREQ,        // clocks (MHz, GHz)
    GK_POWER,       // W, V, Wh
    GK_TEMP,        // degrees C
    GK_MEMORY,      // memory size and bandwidth (MiB, GB/s)
    GK_LATENCY,     // latency / time (ns, us, ms, cycles, frame time)
    GK_DISK,        // disk throughput and IOPS (MB/s, IOPS)
    GK_RATE,        // compute throughput (GFLOPS, Gops/s, fps, Mtri/s)
    GK_OTHER,       // counts and everything else
    GK_COUNT
};
extern const char* const g_graph_kind_names[GK_COUNT];

GraphKind graph_kind_for_unit(const char* unit);   // "%" -> load, "MHz" -> clock, "GB/s" -> memory, ...
u32 graph_rgb(GraphKind k, bool dark);              // 0xRRGGBB of plain Light / Dark, any thread (the overlay)
ImU32 graph_color(GraphKind k, f32 alpha = 1.0f);   // for the current theme
// Color of line `i` of a multi-line graph: line 0 in the kind's color, the others in companion hues.
ImU32 graph_line_color(GraphKind k, u32 i, f32 alpha = 1.0f);

// Value with its unit for display: MHz >= 1000 as GHz, MiB >= 1024 as GiB, "15%". decimals < 0: automatic.
void graph_format(char* buf, u32 size, f64 v, const char* unit, i32 decimals = -1);

enum GraphFlags : u32 {
    GF_SPARK      = 1u << 0,    // thumbnail: no caption, grid, hover or tooltip (a Dummy, so it can sit in a Selectable)
    GF_NO_CAPTION = 1u << 1,    // no caption line above the box
    GF_ZERO       = 1u << 2,    // automatic range includes 0
    GF_VALUE      = 1u << 3,    // current (last) value of line 0 inside the box, top left
    GF_NO_TOOLTIP = 1u << 4,    // the caller shows its own tooltip from the returned GraphHit
    GF_LOG_X      = 1u << 5,    // x/y: logarithmic x (every x > 0)
    GF_POINTS     = 1u << 6,    // x/y: a dot on every point
    GF_NO_FILL    = 1u << 7,    // line only (e.g. x/y data whose x goes back and forth)
};

struct GraphLine {
    const f32* y = nullptr;     // NaN = gap; time series: oldest first
    u32 n = 0;
    const f32* x = nullptr;     // x/y only; null = 0, 1, 2, ...
    const char* name = nullptr; // legend and tooltip (null = none)
    i32 kind = -1;              // color: -1 = the graph's kind (line i > 0: a companion hue)
    const u8* marked = nullptr; // x/y: nonzero = this point drawn in Graph::mark_col (e.g. flagged runs)
};

struct Graph {
    const char* title = nullptr;    // caption above, left ("% Utilization over 60 seconds")
    const char* right = nullptr;    // caption above, right; null: time series show the range max ("100%")
    ImU32 right_col = 0;            // color of `right` (0 = dimmed text), e.g. a warning color
    const char* unit = "";          // unit of the values (caption, tooltip, axis labels)
    GraphKind kind = GK_OTHER;
    u32 flags = 0;                  // GraphFlags
    f32 lo = NAN, hi = NAN;         // fixed range ends; NaN = from the data (rounded to a nice value)
    f32 min_span = 0.0f;            // automatic range: at least this tall (e.g. 10 for temperatures)
    i32 decimals = -1;              // values in tooltip / overlay (-1 = automatic)
    // Time series
    u32 window = 0;                 // samples across the width (0 = the longest line); newest at the right
    f32 step_s = 0.0f;              // seconds per sample, for "12 s ago" in the tooltip (0 = no age)
    // x/y
    const char* x_unit = nullptr;   // format_x() unit ("bytes", "threads") for tick labels and the tooltip
    void (*fmt_x)(char* buf, u32 size, f64 x, const void* user) = nullptr;     // custom x text (dates, passes)
    void (*fmt_y)(char* buf, u32 size, f64 y, const void* user) = nullptr;     // custom y text
    const void* user = nullptr;
    ImU32 mark_col = 0;             // color of marked points
};

struct GraphHit { i32 line = -1, point = -1; };     // hovered point (-1 = none)

// `size` is the whole item including the caption; size.x <= 0 = the available width.
GraphHit graph_time(const char* id, const Graph* g, const GraphLine* lines, u32 count, ImVec2 size);
GraphHit graph_xy(const char* id, const Graph* g, const GraphLine* lines, u32 count, ImVec2 size);

// Horizontal bar `size` at the cursor, vertically centered on a text line: filled to `frac` (0..1) in `col`,
// with a thin marker at `marker` (0..1, e.g. a reference; <= 0 = none).
void graph_bar(f32 frac, f32 marker, ImVec2 size, ImU32 col);

// Task Manager style statistic: a small label (with a dot in the kind's color, kind < 0 = none) over a big value.
void graph_stat(const char* label, const char* value, i32 kind = -1);
