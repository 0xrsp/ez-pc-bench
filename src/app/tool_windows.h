#pragma once
// Secondary top-level windows opened from the main window. Each has its own HWND, swapchain and ImGui
// context (see main_gui.cpp); asking for one that is already open just brings it to the front.
#include "core/types.h"

enum ToolWindow : u8 { TOOL_COMPARE, TOOL_SETTINGS, TOOL_TEMPS, TOOL_HEALTH, TOOL_FRAMES, TOOL_HISTORY,
                       TOOL_SYSTEM, TOOL_COUNT };
