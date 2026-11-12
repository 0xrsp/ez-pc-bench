#pragma once
// Frame-time graph (frame time in ms over capture time in s) for one or more frame captures. Used by the
// frame capture window and the compare window.
#include "core/types.h"
#include "imgui.h"

struct FrameSeries {
    const char* label;
    const f32* ms;          // frame times, in order
    u32 count;
};

ImU32 frame_graph_color(u32 i);     // series color (also for legends)
// Full available width, `height` tall. y axis 0..y_max_ms (taller frames are clipped to the top edge). Each
// pixel column shows the slowest frame that falls into it, so single hitches stay visible.
void frame_graph(const char* id, const FrameSeries* s, u32 n, f32 y_max_ms, f32 height);
