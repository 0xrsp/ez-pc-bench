#pragma once
// Disk health window options in %APPDATA%\ez-pc-bench\health\health.ini, section [ui] (e.g. hide_serial).
#include "core/types.h"

bool health_ui_option(const char* name, bool def);
void health_set_ui_option(const char* name, bool value);
