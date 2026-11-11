#pragma once
#include "core/bench.h"

void ui_init();                         // also loads the saved settings
void ui_shutdown();                     // saves pending settings changes
void ui_start_run();                    // start a run with the current selection/settings
void ui_set_duration(u32 ms);
void ui_select_only(const char* ids);   // dev: comma-separated test ids / prefixes ("smoke.preview,cpu.int")
void ui_set_preview(void* texture, u32 w, u32 h);   // GPU test preview (ImTextureID), null when inactive
void ui_apply_style(f32 dpi_scale);     // (re)build style for a DPI scale
void ui_draw();                         // one frame of the whole UI
bool ui_wants_quit();                   // e.g. after relaunching elevated
