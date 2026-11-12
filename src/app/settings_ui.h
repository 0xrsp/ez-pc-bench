#pragma once
// Settings windows. Hover a setting for details.
//   settings_ui_draw: app settings (theme, threads, GPU present mode, disk options, advanced sensors).
//   bench_settings_ui_draw: the run settings presets set (phase duration, repeat, sustained mode, disk test
//                     file size, passes) plus two that presets never change: GPU render resolution and the
//                     disk interval.

void settings_ui_draw();
void bench_settings_ui_draw();
