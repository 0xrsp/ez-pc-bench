#pragma once
// Compare window contents: any number of result runs side by side, raw values and change vs a reference run.
#include "core/types.h"

void compare_ui_on_open();                      // called when the window opens (adds the current run if empty)
void compare_ui_add_path(const wchar_t* path);  // drag & drop of a results file
void compare_ui_draw();                         // one frame (current ImGui context = the compare window)
