#pragma once
#include "core/bench.h"

void ui_init();
void ui_start_run();                    // start a run with the current selection/settings
void ui_set_duration(u32 ms);
void ui_apply_style(f32 dpi_scale);     // (re)build style for a DPI scale
void ui_draw();                         // one frame of the whole UI
bool ui_wants_quit();                   // e.g. after relaunching elevated
