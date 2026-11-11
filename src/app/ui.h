#pragma once
#include "app/tool_windows.h"
#include "core/bench.h"

void ui_init();                         // also loads the saved settings
void ui_shutdown();                     // saves pending settings changes
void ui_start_run();                    // start a run with the current selection/settings
void ui_set_duration(u32 ms);
void ui_set_sustain(u32 seconds);       // dev: sustained mode for this session (--sustain <minutes>)
void ui_show_charts();                  // dev: open the Charts tab (--charts)
void ui_select_only(const char* ids);   // dev: comma-separated test ids / prefixes ("smoke.preview,cpu.int")
void ui_set_dev(bool dev);              // dev: show the smoke-test suite
void ui_set_preview(void* texture, u32 w, u32 h);   // GPU test preview (ImTextureID), null when inactive
void ui_apply_style(f32 dpi_scale);     // (re)build style for a DPI scale
void ui_draw();                         // one frame of the whole UI
bool ui_wants_quit();                   // e.g. after relaunching elevated
void ui_request_tool(ToolWindow t);     // open (or focus) a tool window after this frame
u32 ui_take_tool_requests();            // bit per ToolWindow requested since the last call
void ui_results_dir(wchar_t* dir);      // "results" folder next to the exe (created on demand), MAX_PATH chars
BenchParams* ui_params();               // the settings the next run uses (settings window edits them)
