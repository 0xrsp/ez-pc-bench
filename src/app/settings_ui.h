#pragma once
// Settings windows. Hover a setting for details.
//   settings_ui_draw: app settings that presets never change (theme, threads, GPU resolution / present mode,
//                     disk options, advanced sensors).
//   bench_settings_ui_draw: the run settings presets set (phase duration, repeat, sustained mode, disk test
//                     file size, passes and interval).

void settings_ui_draw();
void settings_ui_open_theme_combo();    // dev (--open-theme-combo): theme dropdown opens itself (screenshots)
void bench_settings_ui_draw();
