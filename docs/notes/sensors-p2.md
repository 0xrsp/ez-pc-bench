# Live monitoring extras (FUTURE.md §7 P2 / P3)

Stream notes for merging into PLAN.md / FUTURE.md. Built 2026-11-11 on a worktree branch from `dev`.

## Status vs FUTURE.md §7

| Item | State |
|---|---|
| P2 Battery: rate W, voltage, design vs full capacity (wear), cycle count | **Done.** `core/battery.*`, Sensors window (Overview → Power), channels `bat.*`, run monitor `env.bat_discharge_w` |
| P2 Energy Metering Interface power rails | **Done.** `core/emi.*`; this laptop exposes CPU package + 8 per-core rails; "not available" handled with a reason |
| P2 Sensor log CSV / TOON at an interval | **Done.** GUI (Log and overlay tab) and CLI `--log-sensors` |
| P2 min / max / avg with reset | **Done.** Sensors tab, "Reset min / max / avg" |
| P2 per-sensor graphs | **Done.** Every row has a 2-minute graph; the "Picked" view shows 10-minute graphs |
| P2 pick sensors to show | **Done.** "Pick" column → "Picked" view and the log; "OSD" column → overlay. Persisted in `settings.ini [sensors]` |
| P2 Tray icon / threshold alerts | **Skipped**: needs resident mode, an open FUTURE §0 decision |
| P3 Overlay (top-most, click-through, transparent; fps during GPU tests) | **Done.** `app/overlay.*`; no injection, so not over exclusive fullscreen |

## What was built

- `core/battery.*` — battery class driver: SetupAPI enumerates `GUID_DEVCLASS_BATTERY` interfaces,
  `IOCTL_BATTERY_QUERY_TAG`, `_QUERY_INFORMATION` (BatteryInformation, device / manufacturer / serial name,
  temperature probed once) and `_QUERY_STATUS`. No admin needed. `battery_discharge_w()` = summed discharge
  rate, NaN on AC.
- `core/emi.*` — Energy Metering Interface: `GUID_DEVICE_ENERGY_METER` interfaces, `IOCTL_EMI_GET_VERSION /
  _METADATA_SIZE / _METADATA / _MEASUREMENT`, v1 and v2 metadata. Power = Δenergy (pWh) / Δtime (100 ns)
  between two reads. RAPL-style names get readable names and short ids (`_PKG` → "CPU package" /
  `emi.cpu_pkg`, `_CoreN_CORE` → "CPU core N" / `emi.coreN`, `_PP0`, `_PP1`/`_GFX`, `_DRAM`, `_PSYS`);
  sorted package, domains, cores by number. No admin needed. Status text when no meter / access denied /
  no channels.
- `core/sensors.*` (owned by this stream, API extended additively; existing `sensors_snapshot` /
  `sensors_live` / `SensorReading` / `CpuLive` / `GpuLive` unchanged):
  - `sensors_start` / `sensors_stop` are now **reference counted** (window, log and overlay each hold one).
  - The sampler reads every source without the lock and publishes under it (the UI no longer waits behind a
    slow PDH query). Thermal zones moved to their own thread: under full load that PDH query stalls 5–20 s.
  - Flat **channel list** (`SensorChannel`: id, name, short label, unit, group, flags, decimals, now /
    min / max / sum / count) + one-second history ring (600 points = 10 min) per channel.
    `sensors_channels`, `sensors_channel_history`, `sensors_reset_stats`, `sensors_channel_matches`.
  - `sensors_power()` → battery static info + status, EMI rails, EMI status text.
  - Sensor log: `sensors_log_start(path, CSV|TOON, interval_ms, patterns)`, `_stop`, `_status`. The sampler
    samples at the log interval when it is below 1 s (min 100 ms) and runs at HIGHEST priority while logging.
  - Room for PawnIO (or any other source): add a reader to `Sources` / `read_all` and `ch_put(...)` calls in
    `publish()`; the window, log and overlay pick new channels up without changes.
- `app/temps_ui.cpp` — Sensors window with tabs **Overview** (previous content + Power section: battery line,
  name / chemistry / design / full / wear / cycles, EMI rails with per-core grid), **Sensors** (all channels:
  Pick, OSD, name, now, min, max, avg, graph; All / Picked view; per-core rows toggle; reset), **Log and
  overlay** (format, interval, start / stop, row count, "Show in folder"; overlay on/off, corner).
  Logging stops when the window closes (the TOON file gets finished); the overlay stays.
- `app/overlay.*` — layered `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
  WS_EX_NOACTIVATE` popup on its own thread (500 ms refresh, so its timer never wakes the GUI loop). GDI
  draws anti-aliased text into a DIB; gray level → coverage over a translucent dark panel, premultiplied,
  `UpdateLayeredWindow`. Channels in Sensors-list order, `FPS` first while a GPU test reports fps
  (`g_live.unit == "fps"`). Default set when nothing is ticked: CPU load, fastest-core clock, CPU package
  power, temperatures, GPU0 load, battery rate.

## Channel ids (stable on a machine)

`cpu.load`, `cpu.busy_mhz`, `cpu.max_mhz`, `lpN.mhz` / `lpN.load` (detail), `gpuN.core_mhz`, `gpuN.mem_mhz`,
`gpuN.load`, `gpuN.vram_mib`, `gpuN.power_pct`, `gpuN.fan_rpm` (N = `g_sys.gpus` index), `temp.<zone>`
(e.g. `temp.tz01`), `gpuN.temp`, `diskN.temp` (N = physical disk number), `bat.rate_w` (+ charging,
− discharging), `bat.voltage_v`, `bat.charge_pct`, `bat.remaining_wh`, `bat.temp` (only if reported;
`batN.*` with several batteries), `emi.<id>` (`emi.cpu_pkg`, `emi.coreN` detail, ...).
"Detail" channels (per LP / per core) are left out of logs and the list unless picked / asked for.
`ez-pc-bench-cli --list-sensors` prints them with current values.

## File formats

CSV (UTF-8 with BOM so Excel reads it as UTF-8):
```
Time,Seconds,CPU load [%],"CPU clock, busy cores [MHz]",...
2026-11-11 15:07:48.807,0.000,10.5,,4416,...
```
Empty cell = no value at that moment.

TOON (`*.toon.txt`; rows stream to `<file>.rows.tmp`, the header with the row count is written on stop and
the temp file deleted; if writing fails the rows stay in the temp file):
```
app: ez-pc-bench
format: 1
kind: sensor-log
timestamp: "2026-11-11T15:07:43"      # local time of the first row
interval_ms: 500
cpu: AMD Ryzen 7 250 w/ Radeon 780M Graphics
channels[9]{id,name,unit}:
  cpu.load,CPU load,%
  ...
samples[7]{t,cpu.load,cpu.busy_mhz,...}:
  0,17.9,null,4376,...                  # t = seconds since the first row, null = no value
```
Note: our own TOON reader caps tables at `TOON_MAX_FIELDS` = 16 columns; logs are wider, but nothing in the
app reads them back (they are for spreadsheets / scripts). The results file's `env` table is now exactly 16
columns (see below) — the next env column needs `TOON_MAX_FIELDS` raised if the compare / baseline readers
ever parse `env` (today they skip it).

## Flags

- CLI `--log-sensors <file>`: `.csv` → CSV, anything else → TOON. Alone: logs for `--seconds N` (default 60,
  Ctrl+C stops early, prints progress). With `--run` / `--preset`: logs for the length of the run.
  `--interval <ms>` (default 1000, min 100), `--sensors <patterns>` (comma list with `*`, default = every
  non-detail channel, `all` = everything). `--list-sensors` lists channel ids.
- GUI `--overlay`: shows the overlay at start with the saved channels / corner (dev / testing aid).

## Run monitor (shared files, additive)

- `TestEnv` gains `f32 bat_discharge_w` (appended after `flags`): time-weighted average discharge W during
  the test, NaN on AC / no battery. Battery read at most once a second on the monitor thread (0.02–1.7 ms
  per read here).
- TOON `env` table: column `bat_discharge_w` **appended after `flags`** (no reordering). Now 16 columns.
- `env_describe` adds ", battery 20.5 W" to the CPU part when discharging (CLI `[...]` line, GUI hover).

## Measured on this laptop (Lenovo, Ryzen 7 250, 2026-11-11)

- Battery: one, "L24M4PK6", manufacturer "SMP", chemistry LiP, **design 60.00 Wh, full charge 60.00 Wh
  (wear 0.0 %), 59 cycles**; no temperature. On battery at idle-ish desktop load: **−20.49 W at 12.90 V**,
  42.9 % (25.73 Wh). Status refresh by firmware is slow (the same rate for > 6 s of 1 Hz reads).
  On AC at 42 % it reports state ONLINE|CHARGING with rate 0 W (charge threshold / conservation mode) —
  the UI says "On AC, not charging" when the rate is ~0.
- `battery_open` 5.7 ms; `IOCTL_BATTERY_QUERY_STATUS` 0.02–1.7 ms idle, but **2.4 s under all-core load**
  (cpu.int.alu MT phase): the ACPI / EC work runs on starved system threads. Hence `BatteryPoller` (own
  thread, 1 s period); neither the run monitor (TIME_CRITICAL) nor the sensor sampler calls it directly.
  `battery_poll_stop` can take up to one stalled read (~2.4 s) at the end of a run in that case.
- Slowest sensor sample under all-core load before the fix: 3157 ms = cpu (PDH % Processor Performance)
  556 ms + battery 2367 ms + EMI 213 ms + drives 18 ms + GPU 3 ms; idle: 8 ms total. The CLI prints the
  slowest sample's breakdown after every `--log-sensors` run.
- After the fix (battery poller, PCIe link every 10 s, zones on their own thread), same test with a 500 ms
  log: 13 rows in 8.3 s, on cadence through the all-core phase except one 2.2 s gap where the PDH
  "% Processor Performance" collect took 2123 ms (known, PLAN §13; the run monitor has the same stall).
  Package power read a steady ~39.2 W through ST and MT on AC (platform power limit).
- EMI: **9 meter devices, all EMI v2, one channel each, model "PPM"**: `RAPL_Package0_PKG` and
  `RAPL_Package0_Core0_CORE` … `Core7_CORE`. Readable without admin. `emi_open` ~33 ms; a read of all 9
  0.4–6 ms, once 112 ms (isolated). Package power ~7–9 W at desktop idle on battery, 30–43 W while other
  agents' builds ran; per-core 0.0–15 W.
- Overlay: ex-style 0x080800A8 verified; renders over other windows, clicks pass through.

## Not done / follow-ups

- Tray icon and threshold alerts (needs FUTURE §0 resident mode).
- EMI CPU package power is a natural run-monitor column (`cpu_pkg_w`) — not added here to keep the shared
  edit to the battery column; the reader is ready (`core/emi.*`). Note `TOON_MAX_FIELDS` above.
- Overlay position is per primary monitor only; no drag-to-move (it is click-through by design).
- The log records instantaneous values per row (not interval averages).
