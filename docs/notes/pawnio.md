# PawnIO advanced sensors: research, design, status

Stream: FUTURE.md §8 (all) + §4 "SPD and current timings". Researched and written 2026-11-11.
**Nothing here has run against the real driver**: PawnIO isn't installed on the development machine (no
`PawnIO` service, no `\Device\PawnIO`, no uninstall entry, no `%ProgramFiles%\PawnIO`), and installing a
driver is the user's decision. Everything is written to fail closed: without PawnIO the app behaves exactly as
before (checked: CLI `--adv-report` says "not installed", Settings shows "Not installed", runs unchanged).

## 1. License verdict

**We may do what we need.** Talking to the driver with our own `DeviceIoControl` code is explicitly allowed,
and shipping the signed module `.bin` files is allowed under LGPL-2.1 with the usual notices.

| Part | License | Source |
|---|---|---|
| PawnIO driver | GPL-2.0-or-later **with a special exception**: PawnIO may be combined with "independent modules that communicate with PawnIO solely through the device IO control interface". The exception doesn't cover code that talks to it over the Pawn interface (i.e. the modules themselves, which must be license-compatible; LGPL-2.1 recommended). | [PawnIO README](https://github.com/namazso/PawnIO), header of [pawnio_um.h](https://github.com/namazso/PawnIO/blob/master/PawnIO/include/pawnio_um.h) |
| Official signed driver binary / installer | Proprietary binary build of the same source; redistributing the installer is allowed. The maintainer recommends sending users to pawnio.eu. | [Using PawnIO Modules (wiki)](https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules), [pawnio.eu](https://pawnio.eu) |
| PawnIOLib (user-mode library) | LGPL-2.1-or-later | header of [PawnIOLib.cpp](https://github.com/namazso/PawnIO/blob/master/PawnIOLib/PawnIOLib.cpp) |
| PawnIO.Modules (AMDFamily17, RyzenSMU, IntelMSR, SmbusPIIX4, SmbusI801, ...) | LGPL-2.1 | [PawnIO.Modules COPYING](https://github.com/namazso/PawnIO.Modules) |

- **Our client** (`src/core/pawnio.cpp`) is an independent program using only the IOCTL interface, which is
  exactly what the exception names. We don't use PawnIOLib (no LGPL linking question) and we don't ship or
  install the driver, so the driver's GPL terms don't attach to anything we distribute.
- **Module blobs**: the official wiki tells integrators to take the module binaries from a PawnIO.Modules
  release and "include its contents in your software". They are LGPL-2.1 object code: if we ship them, ship
  them unmodified (the driver checks their signature anyway), with the LGPL-2.1 text and a note naming the
  release and its source (that's how LibreHardwareMonitor does it:
  `LibreHardwareMonitorLib/Resources/PawnIo/{*.bin, COPYING, README}`, README: "Signed PawnIO modules from
  the official repository, belonging to the 0.2.11 release"). They run inside the driver, not linked into our
  process; shipping them as separate files keeps them trivially replaceable.
- **Not done in this stream**: no module `.bin` is committed (downloading and vendoring third-party binaries is
  the lead's call). See §6 for how to add them.
- Not legal advice; based on the license texts above as of 2026-11-11.

## 2. HVCI (memory integrity)

No official statement found (searched pawnio.eu, the three repos' READMEs / issues, the web). Evidence it works:
- the driver is an interpreter: modules are signed Pawn bytecode run by an in-driver VM, no code generation
  (no W+X memory, which is what HVCI forbids) — [PawnIO source](https://github.com/namazso/PawnIO/tree/master/PawnIO/src);
- a [PawnIO.Modules issue (#135, 2026-11)](https://github.com/namazso/PawnIO.Modules/issues/135) reporter runs
  it with "Memory Integrity (HVCI): Enabled";
- it ships with LibreHardwareMonitor / FanControl to default Windows 11 installs (HVCI on by default) and is the
  replacement Microsoft's WinRing0 block pushed those tools to; no report of PawnIO on the vulnerable-driver
  blocklist found.

Verdict: very likely compatible, **untested here** (this PC: memory integrity off, VBS on).

## 3. Anti-cheat

- **FACEIT** blocked `PawnIO.sys` in 2025 because its signing certificate was also used by cheat software
  ([PawnIO.Setup#1](https://github.com/namazso/PawnIO.Setup/issues/1),
  [FanControl discussion #3537](https://github.com/Rem0o/FanControl.Releases/discussions/3537), FACEIT's
  "Forbidden driver" support article). The maintainer re-signed: "should be fixed as of 2.1.0" (2026-02-12),
  declared "solved" 2026-05-24.
- **Vanguard, EasyAntiCheat, BattlEye, EA Javelin**: no reports of blocks found (the FanControl discussion asked;
  no answers).
- Consequence for us: opt-in only, never installed by us. Our Settings switch closes our handles, but the
  driver stays loaded as long as PawnIO is installed (PnP device, `SERVICE_DEMAND_START` started by its device),
  so a user who must satisfy an anti-cheat has to uninstall PawnIO, not just switch us off. The Settings help
  text says the driver is the user's to install.

## 4. Driver interface (from PawnIO 2.2.0 source; no library needed)

- Device `\Device\PawnIO`; from Win32 `CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ|GENERIC_WRITE, ...)`
  (the legacy `\\.\PawnIO` link was restored in 2.2.0 "for broken third-party PawnIOLib implementations").
  ACL `D:P(A;;GA;;;SY)(A;;GA;;;BA)` (`PawnIO.inf.in`): **administrators only** (2.2.0 can optionally expose
  it to others).
- Device type 41394 (0xA1B2), all `METHOD_BUFFERED`, `FILE_ANY_ACCESS`:
  - `IOCTL_PIO_LOAD_BINARY` = `CTL_CODE(0xA1B2, 0x821, ...)` = 0xA1B22084: input = the module blob. One module
    per handle (`FileObject->FsContext`); a second load on the same handle fails `STATUS_ALREADY_INITIALIZED`.
    The official build verifies the module signature; the module's `main()` can refuse the hardware
    (`STATUS_NOT_SUPPORTED`).
  - `IOCTL_PIO_EXECUTE_FN` = 0xA1B22104: input = 32-byte zero-padded ASCII function name + `u64` input cells;
    output = `u64` cells.
  - `IOCTL_PIO_VERSION` = 0xA1B22184 (4-byte output).
- Installed-version detection: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO`, `DisplayVersion`
  (as LibreHardwareMonitor `PawnIo/PawnIo.cs`, which also uses plain `DeviceIoControl`).
- Cross-tool bus mutexes (LibreHardwareMonitor `Hardware/Mutexes.cs`, module doc comments):
  `Global\Access_PCI` (SMN / SMU index-data pairs), `Global\Access_SMBUS.HTP.Method` (SMBus). We take them
  around every transaction batch and skip the sample on timeout.

Module functions we call (all reads; never `ioctl_write_*`, `ioctl_send_smu_command`, `ioctl_piix4_port_sel`,
`ioctl_clock_freq`):

| Module | Function | in → out |
|---|---|---|
| AMDFamily17 | `ioctl_read_msr` | [msr] → [value] (allow-listed MSRs, executes on the calling thread's CPU) |
| AMDFamily17 | `ioctl_read_smn` | [address] → [value] (PCI 00:00.0 0x60/0x64) |
| RyzenSMU | `ioctl_resolve_pm_table` | [] → [version, base] |
| RyzenSMU | `ioctl_update_pm_table`, `ioctl_read_pm_table` | [] → [], [] → [table qwords, ≤ 512] |
| IntelMSR | `ioctl_read_msr` | [msr] → [value] |
| SmbusPIIX4 / SmbusI801 | `ioctl_smbus_xfer` | [addr, 1 = read, command, protocol 2 byte / 3 word] → [data] |

## 5. Design

Files (all new except the hooks listed below):
- `src/core/pawnio.{h,cpp}` — detection, blob lookup (RCDATA `PAWNIO_<NAME>` or `<exe>\pawnio\<Name>.bin`),
  open/load, call, bus mutexes.
- `src/core/adv_sensors.{h,cpp}` — public API (below), status, the Settings switch, the one sampling thread,
  snapshots, CLI report. `adv_internal.h` shares the handles with:
- `src/core/adv_cpu.cpp` — AMD Tctl/Tdie, CCDs, SVI2, RAPL, PM table, APERF/MPERF, clock stretching; Intel
  package / core DTS, RAPL (package, PP0, PP1), APERF/MPERF, limit status bits.
- `src/core/adv_smbus.cpp` — SPD5118 / TSE2004 temperatures, read-only SPD base configuration.
- `src/core/adv_umc.cpp` — AMD UMC timings and clocks.
- `src/core/sysinfo_spd.{h,cpp}` — `g_memdetail` (SPD + timings), one-line formatting, TOON output.
- `src/app/adv_ui.{h,cpp}` — Sensors-window section (temperature table with history, power / voltage / clocks
  lines, per-LP clocks on hover) and the Settings row.

API for other streams (`core/adv_sensors.h`):
- `adv_start()` / `adv_stop()` — reference counted; the first user starts the 1 Hz thread (no-op without PawnIO).
- `adv_cpu_temp(max_age_s)` — latest CPU temperature (AMD Tdie / Intel package) or NaN. **This is what the
  stability tester should use for its safety limit** (call `adv_start()` when its run starts, `adv_stop()` at
  the end; treat NaN as "no CPU temperature available").
- `adv_cpu_snapshot(AdvCpu*)`, `adv_mem_snapshot(AdvMem*)` — everything else; `adv_status(AdvInfo*)` for UI text.

Polling rules (FUTURE §8 P2), enforced in `adv_sensors.cpp`:
- one sampling thread, 1 s tick, `THREAD_PRIORITY_TIME_CRITICAL` like the run monitor (short bursts that must
  not starve behind HIGHEST benchmark threads);
- SMU PM table at most once per tick (≤ 1 Hz, AMD's guidance for its monitoring SDK);
- while `runner_is_running()`: no per-LP hopping (APERF/MPERF, Intel per-core DTS) and no SMBus traffic;
  only package-scope reads (one SMN read or one MSR, RAPL, PM table);
- DIMM sensors every 2 s when idle; a DIMM that fails 3 times in a row is dropped; any SMBus timeout, collision
  or busy answer stops all SMBus use for the session (EC-owned bus on laptops).

Hooks in shared files (small, additive):
- `CMakeLists.txt`: 6 new core sources, `src/app/adv_ui.cpp`, generated `pawnio_modules.rc` in both exes
  (embeds `third_party/pawnio/*.bin` when present; empty otherwise).
- `src/core/monitor.{h,cpp}`: `adv_start()` / `adv_stop()` in `monitor_begin` / `monitor_end`; `cpu_temp` =
  `adv_cpu_temp(2.5)` → `TestEnv::cpu_temp_max` and the existing `ENV_CPU_HOT` flag now fill in with PawnIO.
- `src/core/results.cpp`: `memdetail_write_toon(f, 2)` → `system.ram.timings` / `system.ram.spd[...]`
  (nothing written without data; the compare / baseline readers skip unknown keys and tables).
- `src/app/temps_ui.cpp`: `adv_ui_sensors_open/close` + `adv_ui_draw_sensors()` after the drives table; footnote.
- `src/app/settings_ui.cpp`: "Sensors" group with the row; also fixed the existing
  `IsWindowHovered(ImGuiHoveredFlags_AllowWhenDisabled)` call, which made ImGui show a red error tooltip over
  the Settings window in release builds (separate commit).
- `src/app/settings.cpp`: `adv_sensors=0|1` in `[settings]` (default 1).
- `src/app/ui.cpp`, `src/app/main_cli.cpp`: timings line under RAM; CLI `--adv-report`, `--no-adv-sensors`;
  `adv_init()` in both mains.

## 6. Enabling it for a test (user / lead steps; nothing automatic)

1. The user installs PawnIO from https://pawnio.eu (official installer), reboots if asked.
2. Download `release_0_2_11.zip` (or newer) from
   https://github.com/namazso/PawnIO.Modules/releases and put `AMDFamily17.bin`, `RyzenSMU.bin`,
   `SmbusPIIX4.bin` (Intel: `IntelMSR.bin`, `SmbusI801.bin`) either into `third_party/pawnio/` (re-run CMake:
   embedded) or into `build/release/pawnio/` next to the exes. If shipped, add the COPYING (LGPL-2.1) and a
   README naming the release.
3. Elevated: `build/release/ez-pc-bench-cli.exe --adv-report` (status, one sample, DIMM sensors, SPD, timings,
   and a raw dump of the PM table for mapping Hawk Point's layout), then the GUI Sensors window.

## 7. Register sources (cited in the code, not copied)

- AMD: LibreHardwareMonitor (MPL-2.0) `Hardware/Cpu/Amd17Cpu.cs` (Tctl 0x59800, CCD bases 0x59954 / 0x59B08 /
  0x599F0, SVI2 0x5A000 + plane offsets, RAPL), Linux `drivers/hwmon/k10temp.c` (range-select / TJ_SEL −49 °C,
  per-part Tdie offsets), AMD PPR 55570 / 57896 (P-state and RAPL MSRs).
- PM tables: RyzenAdj (LGPL-3.0) `lib/api.c` (Renoir, Cezanne, Phoenix / Hawk Point Tctl + PPT, Strix Point /
  Halo, table sizes); LibreHardwareMonitor `Hardware/RyzenSMU.cs` / `Amd17Cpu.cs` (Raven, Matisse, Vermeer,
  Raphael, Granite Ridge indices).
- UMC timings: ZenTimings / ZenStates-Core (GPL-3.0) — `MainWindow.xaml.cs ReadTimings()`,
  `Hardware/DRAM/MemoryConfig.cs`, `Ddr5Timings.cs`: offsets 0x50100 (type), 0x50200 (ratio, 2T, GDM),
  0x50204-0x50234 (timings), 0x50260-0x5026C (tRFC), 0x502C0-0x502CC (tRFCsb), channel = 0x50000 + (n << 20),
  0x50DF0 bit 19 (channel disabled), chip-select bases 0x50000 + 4·cs bit 0 (DIMMs / ranks).
- Intel: SDM vol. 4 (IA32_THERM_STATUS 0x19C, IA32_PACKAGE_THERM_STATUS 0x1B1, TEMPERATURE_TARGET 0x1A2,
  PLATFORM_INFO 0xCE, RAPL 0x606 / 0x611 / 0x639 / 0x641), LibreHardwareMonitor `IntelCpu.cs`.
- DIMMs: JEDEC JESD300-5 (SPD5118: MR0/1 type, MR5 caps, MR11 page / addressing, MR26 sensor disable, MR49/50
  temperature [12:2] × 0.25 °C), JESD400-5 (DDR5 base bytes 0-127), SPD annex L (DDR4), JC-42.4 / TSE2004
  (register 5, MSB first, 1/16 °C); ZenStates-Core `Spd5118Registers.cs`, `Ddr5BaseDecoder.cs`; Linux
  `spd5118.c` / `jc42.c`.
- Module interfaces: PawnIO.Modules `*.p` doc comments.

## 8. FUTURE.md items

| Item | State |
|---|---|
| §8 P1 licenses / HVCI / anti-cheat | **Done** (this file). |
| §8 P1 integration (detect, Settings row with installer link, plain DeviceIoControl, works without it) | **Done**, untested against the driver. Link is text + "Copy link" (no network calls). |
| §8 P1 AMD Zen: Tctl/Tdie, per-CCD, package power (RAPL), effective clock (APERF/MPERF), SVI telemetry | **Done (code)**, untested. SVI2 only for Zen 1-3; Zen 4+ is SVI3 (not decoded): voltage comes from the PM table where mapped. |
| §8 P1 AMD PM table per-core temps / clocks | **Partial**: framework + layouts for Raven, Renoir, Cezanne, Strix Point / Halo, Matisse, Vermeer, Raphael, Granite Ridge. **Phoenix / Hawk Point (this PC, 0x4C000x): only Tctl + PPT are publicly mapped**; `--adv-report` dumps the table to map per-core fields once PawnIO is on this PC. |
| §8 P1 Intel: core / package temps, RAPL, effective clock, limit reasons | **Done (code)**, untested. Limit reasons from the THERM_STATUS "status" bits (current state); the perf-limit-reason MSRs (0x64F / 0x6B0 / 0x6B1) aren't on IntelMSR's read allow-list. |
| §8 P1 RAM: SPD5118 / DDR4 temperatures, SPD contents, EC-locked SMBus | **Partial**: temperatures and the SPD base configuration (type, density, dies, width, DDR4 ranks, JEDEC speed / CL / tRCD / tRP / tRAS / tRC / tRFC) done, read-only. Manufacturer / part come from SMBIOS. **XMP / EXPO and the DDR5 manufacturing block are not read**: they are on other SPD pages and selecting a page is an SMBus write (MR11 on SPD5118, SPA0/SPA1 on DDR4), which this stream must not do. Needs a lead decision (§9). EC-locked / shared bus handled by stopping on the first bus error. |
| §8 P2 AMD UMC timings, MCLK / FCLK / UCLK, gear | **Done (code)** for DDR4 / DDR5 (LPDDR not decoded); FCLK / UCLK only where the PM table layout is mapped (not on Hawk Point yet). Stored in results (`system.ram.timings`) and shown under RAM. |
| §8 P2 Intel MCHBAR timings | **Skipped**: register layouts differ per generation and nothing here can test them; IntelMCHBAR module is ready for it (`ioctl_read_dword`). |
| §8 P2 Super I/O fans / voltages / temps | **Skipped**: entering Super I/O configuration mode and selecting HWM banks are port writes (LpcIO `ioctl_superio_outb` / `ioctl_pio_outb`), and this laptop has an EC, not a Super I/O, to test on. |
| §8 P2 polling rules | **Done** (§5). |
| §8 P3 clock-stretching detection | **Done (code)**, AMD: worst (requested P-state clock − APERF/MPERF clock) of cores ≥ 90 % in C0; only while no benchmark runs (needs per-core hopping). |
| §4 P2 SPD and current timings in sysinfo, stored with every result | **Partial** as above (timings yes; SPD base config yes; XMP / EXPO no). |

## 9. Open questions for the lead

1. **SPD page selection** (one SMBus write to the hub's MR11 per page, restored afterwards; what HWiNFO,
   CPU-Z, ZenTimings and Linux `spd5118` do) would unlock XMP / EXPO, DDR5 ranks / manufacturing data. Allow it?
2. **Ship the module blobs?** LGPL-2.1 allows it (§1). Embedded (`third_party/pawnio/`) or a `pawnio\` folder
   next to the exe (easier to swap, closer to LGPL's spirit). Either needs COPYING + a README with the release.
3. Hawk Point PM table: map per-core fields from an `--adv-report` dump once PawnIO is installed here.
4. Stability tester: use `adv_cpu_temp()`; decide whether SMBus DIMM polling may run during stress tests
   (currently only "no benchmark running" is checked via `runner_is_running()`).
