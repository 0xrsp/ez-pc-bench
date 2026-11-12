# Live monitoring extras (FUTURE.md §7 P2 / P3)

Stream notes for merging into PLAN.md / FUTURE.md. Built 2026-11-11 on a worktree branch from `dev`.

## Status vs FUTURE.md §7

| Item | State |
|---|---|
| P2 Battery: rate W, voltage, design vs full capacity (wear), cycle count | **Done.** `core/battery.*`, Sensors window (Overview → Power), channels `bat.*`, run monitor `env.bat_discharge_w` |
| P2 Energy Metering Interface power rails | **Done.** `core/emi.*`; this laptop exposes CPU package + 8 per-core rails; "not available" handled with a reason. CPU rails shown in the CPU section / group (trim, below) |
| P2 Sensor log CSV at an interval | **Done.** GUI (Log and overlay tab) and CLI `--log-sensors`. The TOON log format: **Removed 2026-11-11 (trim)** |
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
  - Sensor log: `sensors_log_start(path, interval_ms, patterns)` (CSV only since the trim), `_stop`, `_status`. The sampler
    samples at the log interval when it is below 1 s (min 100 ms) and runs at HIGHEST priority while logging.
  - Room for PawnIO (or any other source): add a reader to `Sources` / `read_all` and `ch_put(...)` calls in
    `publish()`; the window, log and overlay pick new channels up without changes.
- `app/temps_ui.cpp` — Sensors window with tabs **Overview** (previous content + Power section: battery line,
  name / chemistry / design / full / wear / cycles, EMI rails with per-core grid), **Sensors** (all channels:
  Pick, OSD, name, now, min, max, avg, graph; All / Picked view; per-core rows toggle; reset), **Log and
  overlay** (interval, start / stop, row count, "Show in folder"; overlay on/off, corner).
  Logging stops when the window closes; the overlay stays. (Layout after the trim: see below.)
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
Groups: the CPU rails (`emi.cpu_pkg`, `emi.cpu_pp0`, `emi.coreN`) are in the **CPU** group, emitted right after
`cpu.max_mhz` (so log columns and the overlay list them next to the CPU clocks); battery and the other rails
(`emi.gfx`, `emi.dram`, `emi.psys`, unknown ones) stay in **Power**.
"Detail" channels (per LP / per core) are left out of logs and the list unless picked / asked for.
`ez-pc-bench-cli --list-sensors` prints them with current values.

## File formats

CSV (UTF-8 with BOM so Excel reads it as UTF-8):
```
Time,Seconds,CPU load [%],"CPU clock, busy cores [MHz]",...
2026-11-11 15:07:48.807,0.000,10.5,,4416,...
```
Empty cell = no value at that moment.

TOON — **Removed 2026-11-11 (trim)**: the log is CSV only (format choice, `log_format` ini key, writer and
`.rows.tmp` handling gone). What it was (`*.toon.txt`; rows streamed to `<file>.rows.tmp`, the header with the
row count written on stop and the temp file deleted):
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
Note: our own TOON reader caps tables at `TOON_MAX_FIELDS` = 16 columns (sensor logs were wider; moot since
the TOON log was removed). The results file's `env` table is now exactly 16
columns (see below) — the next env column needs `TOON_MAX_FIELDS` raised if the compare / baseline readers
ever parse `env` (today they skip it).

## Flags

- CLI `--log-sensors <file>`: always CSV, written under the name as given (also `x.txt`; no `.csv` appended).
  Alone: logs for `--seconds N` (default 60,
  Ctrl+C stops early, prints progress). With `--run` / `--preset`: logs for the length of the run.
  `--interval <ms>` (default 1000, min 100), `--sensors <patterns>` (comma list with `*`, default = every
  non-detail channel, `all` = everything). `--list-sensors` lists channel ids.
- GUI `--overlay`: shows the overlay at start with the saved channels / corner (dev / testing aid).
- GUI `--sensor-filter <text>`: presets the Sensors window search box (dev / screenshot aid; with
  `--tool sensors`).

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
- Overlay: ex-style 0x080800A8 verified; renders over other windows, clicks pass through. During
  `gpu_vk.3d.scene` (GUI `--overlay --autorun`) it showed "FPS 6411" as the first line, gone after the test.

## Not done / follow-ups

- Tray icon and threshold alerts (needs FUTURE §0 resident mode).
- EMI CPU package power is a natural run-monitor column (`cpu_pkg_w`) — not added here to keep the shared
  edit to the battery column; the reader is ready (`core/emi.*`). Note `TOON_MAX_FIELDS` above.
- Overlay position is per primary monitor only; no drag-to-move (it is click-through by design).
- The log records instantaneous values per row (not interval averages).

## Trim 2026-11-11

- **Sensor log: CSV only.** Removed the CSV / TOON choice in the Log tab, the `[sensors] log_format` ini key
  (deleted from the ini on the next save), `SensorLogFormat` / `SensorLogStatus.format`, the TOON writer and its
  `.rows.tmp` temp file. `--log-sensors` writes CSV whatever the extension, keeping the name as given.
  Checked: `--log-sensors x.csv --seconds 3` and `--log-sensors x.txt --seconds 3` both write the same CSV
  (BOM, header, 4 rows x 24 channels).
- **Search box** on the Overview and Sensors tabs (tab row, right of the tabs; Ctrl+F focuses, Esc or the ×
  button clears; not saved, cleared when the window closes). Case-insensitive substring; several
  space-separated words must all match. A channel matches on its name, id or group name ("CPU", "Graphics",
  "Temperatures", "Power"), so `temp` lists every temperature and `cpu power` the CPU rails. Sensors tab: rows
  filtered (the "Per-core rows" toggle still applies; "(N more per-core rows match)" when it hides matches),
  "No sensor matches" when empty. Overview: CPU summary / LP cells / CPU rails, GPU rows, battery, other
  rails and temperature rows are filtered through their channels; sections without a match are hidden; the
  PawnIO section (adv_ui, not filtered row by row) shows when the search matches its topics.
- **CPU power with the CPU.** `EmiChannel.cpu` (package, PP0, per-core rails) and `PowerLive.rail_cpu /
  rail_id`. Overview: the CPU section is "CPU clock, load and power": package power on the load / clock line,
  "Power per core" grid under the LP grid. "Power" keeps the battery and non-CPU rails (graphics, DRAM, PSYS,
  unknown); "Power rails: <reason>" there when the platform has no meter. Sensors tab: CPU rails in the CPU
  group (per-core rails stay detail rows).
