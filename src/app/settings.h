#pragma once
// GUI settings persisted in %APPDATA%\ez-pc-bench\settings.ini (Win32 profile API, UTF-16).
#include "app/theme.h"
#include "core/bench.h"

ThemeMode settings_load_theme();                        // before the window exists; System if unset
void settings_load(BenchParams* p, bool* selected);     // after sysinfo_init; invalid / unknown entries are ignored
// Once per frame from the UI thread: saves (at most once per second) after the settings or selection changed.
// Values that command-line flags overrode at startup are not written unless the user changes them.
void settings_tick(const BenchParams* p, const bool* selected);
void settings_flush(const BenchParams* p, const bool* selected);   // at exit: save pending changes now
