# UI: settings split, bigger font, GPU combo, PawnIO download

Stream written 2026-11-11. Files: `src/app/settings_ui.{h,cpp}`, `ui.{h,cpp}`, `main_gui.cpp`, `tool_windows.h`,
`theme.{h,cpp}`, `adv_ui.{h,cpp}`, `src/core/adv_sensors.{h,cpp}`, `CMakeLists.txt`.

## 1. Settings split in two windows

The rule: a setting that presets (`preset_apply`, `core/presets.cpp`) write is a **benchmark setting**;
everything else is an **app setting**.

| Window | Opened from | Settings |
|---|---|---|
| **Settings** (`TOOL_SETTINGS`, `--tool settings`, 680 x 720) | gear button, right end of the System row | Theme; Threads; GPU render resolution and present mode; disk write-through, read / write passes + mixed-pass share, test data*, disk results unit, test folder, buffered I/O, sustained-write amount; custom disk rows*; advanced sensors (PawnIO) |
| **Benchmark settings** (`TOOL_BENCH`, `--tool bench`, 680 x 580) | new sliders icon button right after Run / Cancel | Duration per phase, repeat each test, sustained mode; disk test file size, passes, interval between tests |

\* rows the disk-trim stream deletes; they were left where they were.

- Presets write duration, repeat, disk passes, disk file size and sustain. The disk interval isn't written by
  presets but was asked to live with the run settings, so it moved too.
- Read / write passes, the mixed-pass share and the sustained-write amount stayed in Settings (presets don't
  touch them). Easy to move later: they are rows in `draw_disk_options` (`settings_ui.cpp`).
- The Benchmark settings window says that presets change these values and shows the current preset
  (`preset_match`, "Custom" when none matches).
- Both windows share the row / group / whole-row help code (one file, `settings_ui.cpp`) and are locked while a
  run is in progress.
- **Reset to defaults** in each window resets only its own settings. Settings: threads, GPU resolution / present
  mode, write-through, disk options except the folder, the custom rows and the interval. Benchmark settings:
  duration, repeat, sustain, file size, passes, interval (= the Standard preset's values). Neither touches the
  theme, advanced sensors, the test selection or the GPU / drives picked in the main window.
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

## 4. PawnIO "Download & install"

Settings > Sensors > Advanced sensors (PawnIO), when PawnIO is not installed: "Not installed" + **Download &
install** (was a "(https://pawnio.eu) [Copy link]").

Flow (`adv_ui.cpp`, worker thread; the UI thread only reads an atomic state):
1. Click -> state *Downloading*: the row shows "Downloading the PawnIO installer..." (animated dots; no
   percentage: `URLDownloadToFileW` progress needs a COM callback object, not worth it for ~3.4 MB).
2. `URLDownloadToFileW(ADV_PAWNIO_SETUP_URL)` =
   `https://github.com/namazso/PawnIO.Setup/releases/latest/download/PawnIO_setup.exe` into
   `%TEMP%\ezpcbench_pawnio\PawnIO_setup.exe`.
3. Signature check before anything runs: `WinVerifyTrust` (valid, trusted Authenticode) **and** the signing
   certificate's organization must be `namazso` (PawnIO's author; checked on the real file:
   `E=admin@namazso.eu, CN=namazso.eu, O=namazso`). Otherwise an error line and nothing is started.
4. `ShellExecuteExW` (verb "open"): the installer's manifest is `requireAdministrator`, so Windows elevates it
   (no UAC prompt when EzPcBench already runs as admin). State *Installer running*.
5. Wait for it to exit, delete the file and the folder, `adv_redetect()` (new in `adv_sensors`: re-runs the
   existing detection and reopens the modules, restarting the sampling thread if the Sensors window uses it).
   The row then shows the normal on / off switch, or "Not installed" again with "The installer closed without
   installing PawnIO." (or the exit code).
6. Errors are shown in the error color under the button (download HRESULT, rejected signature, administrator
   rights refused, couldn't start); the button stays for a retry.

**Network access:** this is the app's first and only network access, and it only happens on that explicit
click. Nothing is contacted at startup or in the background. The row's help text says so.

CLI: `--adv-report` "not installed" now also prints `installer: <ADV_PAWNIO_SETUP_URL>`; the detail line
points to the GUI button. New GUI link libraries: `urlmon wintrust crypt32` (Windows SDK).

Tested: download + signature check through the app's own code (a temporary, uncommitted build that skipped
`ShellExecuteExW`): 3 410 960 bytes, signature by namazso accepted, file and folder deleted afterwards. **Not
tested: running the installer** (the user hand-tests the real install), the "administrator rights refused"
path, and the redetect after a real install.

## 5. Screenshots

In the stream's scratchpad (`shots\`): `main_light/dark`, `main_results_light/dark` (smoke run),
`gpucombo_light/dark` (popup open), `gpucombo2_light` / `gpu2_preview_dark` (both GPUs ticked, via a temporary
ini edit that was restored), `settings_light/dark`, `bench_light/dark`, `pawnio_row_light`,
`pawnio_downloading_dark`, `pawnio_dltest_result_light`, `tool_*` (other tool windows at their default sizes).
