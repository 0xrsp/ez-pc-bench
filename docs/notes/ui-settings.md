# UI: settings split, bigger font, GPU combo

Stream written 2026-11-11. Files: `src/app/settings_ui.{h,cpp}`, `ui.{h,cpp}`, `main_gui.cpp`, `tool_windows.h`,
`theme.{h,cpp}`, `CMakeLists.txt`. (A fourth part, a download button for an optional kernel driver, was removed
2026-11-12 together with that driver.)

## 1. Settings split in two windows

The rule: a setting that presets (`preset_apply`, `core/presets.cpp`) write is a **benchmark setting**;
everything else is an **app setting**.

| Window | Opened from | Settings |
|---|---|---|
| **Settings** (`TOOL_SETTINGS`, `--tool settings`, 680 x 720) | gear button, right end of the System row | Theme; Threads; GPU present mode; disk write-through, read / write passes + mixed-pass share, test data*, disk results unit, test folder, buffered I/O, sustained-write amount; custom disk rows* |
| **Benchmark settings** (`TOOL_BENCH`, `--tool bench`, 680 x 580) | new sliders icon button right after Run / Cancel | Duration per phase, repeat each test, sustained mode; GPU render resolution (moved here 2026-11-12); disk test file size, passes, interval between tests |

\* rows the disk-trim stream deletes; they were left where they were.

- Presets write duration, repeat, disk passes, disk file size and sustain. The disk interval isn't written by
  presets but was asked to live with the run settings, so it moved too. Same for the GPU render resolution
  (moved 2026-11-12): presets still never change it, and it doesn't affect `preset_match`.
- Read / write passes, the mixed-pass share and the sustained-write amount stayed in Settings (presets don't
  touch them). Easy to move later: they are rows in `draw_disk_options` (`settings_ui.cpp`).
- The Benchmark settings window says that presets change these values and shows the current preset
  (`preset_match`, "Custom" when none matches).
- Both windows share the row / group / whole-row help code (one file, `settings_ui.cpp`) and are locked while a
  run is in progress.
- **Reset to defaults** in each window resets only its own settings. Settings: threads, GPU present
  mode, write-through, disk options except the folder, the custom rows and the interval. Benchmark settings:
  duration, repeat, sustain, render resolution, file size, passes, interval (= the Standard preset's values). Neither touches the
  theme, the test selection or the GPU / drives picked in the main window.
- **ini unchanged**: same file (`%APPDATA%\ez-pc-bench\settings.ini`), same `[settings]` keys; `settings.cpp`
  wasn't touched.
- New icon `ICON_SLIDERS` (theme.h) for the toolbar button.

## 2. Bigger font

`theme_apply_style`: `FontSizeBase` 15 -> 16.5 px before DPI (x1.1), snapped to whole pixels: 17 px at 100 %,
21 px at 125 % (was 19). Every other size (numbers font x0.92, headings, `theme_font_size` multiples, the
paddings) is relative to it or unchanged, so all windows (main and every tool window) grow together.

Checked at 125 % (screenshots below): toolbar, System row with the tool buttons, targets bar, test tree,
results table, Settings, Benchmark settings, Compare, Sensors, Disk health, History, System details,
Stability. Nothing clipped. Changes: Settings default 640 x 600 -> 680 x 720, Sensors 1000 -> 1100 wide (its
GPU table's name column cut "NVIDIA GeForce RTX 5060 Laptop GPU" short at the old width). Frame capture wasn't
looked at (the frames stream removes it).

## 3. GPU selection combo (targets bar)

- Rows use `theme_checkbox` (outline only when unticked, filled primary with a tick when ticked) instead of
  plain `ImGui::Checkbox`, whose unticked box was filled with `FrameBg`.
- Compact popup: vertical window padding = FramePadding.y instead of WindowPadding.y; row pitch = a normal
  combo item (font + ItemSpacing.y). Width = checkbox + the longest adapter name + the VRAM column
  ("7.7 GiB VRAM", dimmed, aligned), or the combo's width if that is wider (ImGui's minimum).
- Preview: one adapter = its full name; several = "2 GPUs: RTX 5060 Laptop GPU, Radeon 780M Graphics"
  (vendor words and (TM) / (R) dropped, `gpu_display_name` in ui.cpp). The combo is sized to the preview.
- Dev flag `--open-gpu-combo`: opens the popup on the 3rd frame (for screenshots; the capture script can't
  click).

## 4. Screenshots

In the stream's scratchpad (`shots\`): `main_light/dark`, `main_results_light/dark` (smoke run),
`gpucombo_light/dark` (popup open), `gpucombo2_light` / `gpu2_preview_dark` (both GPUs ticked, via a temporary
ini edit that was restored), `settings_light/dark`, `bench_light/dark`, `tool_*` (other tool windows at their default sizes).
