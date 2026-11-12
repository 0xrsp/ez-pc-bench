# Disk health (FUTURE.md §1, CrystalDiskInfo parity)

Status of the §1 stream. Merge the decisions and measured facts below into PLAN.md and tick FUTURE.md §1.

**Removed 2026-11-11 (trim):** the History tab (history store, samples, plots, endurance projection), the Thresholds & features
tab (per-drive adjustable thresholds and their persistence in `health.ini`) and the AAM / APM controls (ATA SET
FEATURES path) were removed. Status uses fixed CrystalDiskInfo-style Caution limits (any 05 / C5 / C6 sector or NVMe
media error, <= 10 % life left). The window keeps the S.M.A.R.T., Self-test and NVMe details tabs; `--health` is
unchanged apart from no longer writing history samples. Parts below that describe the removed features are marked.

## What was built

| File | What |
|---|---|
| `src/core/disk_health.*` | Orchestration: reads each drive, builds the SMART table (ATA attributes, or the NVMe health log as rows 01–1D like CDI), info panel fields, Good / Caution / Bad, self-test start / poll, text report (AAM / APM: Removed 2026-11-11 (trim)). |
| `src/core/ata.*` | ATA commands over every transport: `IOCTL_ATA_PASS_THROUGH`, legacy SMART IOCTLs, SAT ATA PASS-THROUGH (16) / (12), JMicron 0xDF, Prolific, Sunplus 0xF8, Cypress ATACB 0x24, CSMI. IDENTIFY / SMART-sector parsing. Query-only paths (no admin): IDENTIFY + READ LOG EXT via `IOCTL_STORAGE_QUERY_PROPERTY` (`ProtocolTypeAta`), `SMART_RCV_DRIVE_DATA_EX`. |
| `src/core/scsi_pt.*` | `IOCTL_SCSI_PASS_THROUGH` wrapper (USB bridges). |
| `src/core/nvme_dev.*` | NVMe Identify / Get Log Page / Get Features / Device Self-test: stornvme (`IOCTL_STORAGE_QUERY_PROPERTY`, self-test via `IOCTL_STORAGE_PROTOCOL_COMMAND`) or USB NVMe bridges: JMicron JMS583 (3-phase A1 CDB, also non-data so self-tests work), ASMedia ASM2362 (0xE6), Realtek RTL9210 (0xE4). CDB layouts follow smartmontools' `scsinvme.cpp`. |
| `src/core/csmi.*` | RAID members (Intel RST / AMD / LSI in RAID mode) via CSMI `IOCTL_SCSI_MINIPORT` on `\\.\ScsiN:` (GET_PHY_INFO, STP pass-through). Only probed when a disk reports `BusTypeRAID`. |
| `src/core/smart_attr.*` | Static attribute tables: generic, generic SSD, Samsung, Crucial / Micron, Intel, WD / SanDisk SSD, Phison (Kingston, Patriot, Corsair, PNY...), Seagate; raw-value units (count, hours, temperature with min / max, LBAs, 32 MiB, GiB, life %), vendor picked from the model string. |
| `src/core/health_store.*` | Window options (`health.ini` `[ui]`). Per-drive thresholds, history (TOON) and endurance projection: Removed 2026-11-11 (trim). |
| `src/core/physdisk.*` | Added: `physdisk_location` (SetupAPI parent walk: USB VID/PID of the enclosure bridge, PCIe link current / max and controller name), `physdisk_partitions` (layout + volumes, letters, FS, labels), `physdisk_adapter`, `physdisk_flags` (TRIM / write cache / seek penalty). Existing API unchanged (sensors uses it). |
| `src/app/health_ui.cpp` | Rewritten window: drive chips (status dot, letter, temperature), header with Health status / Temperature boxes and reasons, info grid, tabs S.M.A.R.T. / Self-test / NVMe details (History and Thresholds & features: Removed 2026-11-11 (trim)); Copy report, Hide serial (Save report... removed 2026-11-12). |

Shared-file edits (all small, additive):
- `CMakeLists.txt`: 6 new `ezbench_core` sources after `disk_health.cpp` (ata, nvme_dev, scsi_pt, csmi, smart_attr, health_store). No new libraries (setupapi, shell32, ole32, comdlg32 were already linked).
- `src/app/main_cli.cpp`: `--health [--hide-serial]` prints the text report of every drive and exits (`print_health`, one include, two flags, usage line).
- `src/app/main_gui.cpp`: disk health tool window default size 980x720 -> 1080x880 (one number pair in `s_tool_desc`).
- CLAUDE.md could list `--health` next to `--list` / `--compare` (not edited here).

## Files and settings

- `%APPDATA%\ez-pc-bench\health\health.ini` (UTF-16, profile API): `[ui] hide_serial=0|1`. The per-drive sections with
  `reallocated`, `pending`, `uncorrectable`, `media_errors`, `life_left_pct` are no longer read or written (Removed 2026-11-11 (trim);
  existing ones are ignored).
- Removed 2026-11-11 (trim), no longer written (existing files are left alone): `%APPDATA%\ez-pc-bench\health\<key>.toon.txt`: history. `drive:` object (`model`, `key`, `version: 1`) and
  `samples[N]{time,temp_c,life_used_pct,written_gb,read_gb,power_on_h,reallocated,pending,uncorrectable,media_errors}`
  (Unix seconds; `null` = not reported). Rewritten whole (temp file + rename); at most 4096 samples, oldest dropped.
- Drive key (Removed 2026-11-11 (trim) with the history and thresholds): model with non `[A-Za-z0-9-]` runs turned into `_`, plus `_` + FNV-1a-32 of `model|serial` (the serial
  never appears in a file name), e.g. `SAMSUNG_MZAL81T0HFLB-00BL2_f5962e54`.
- Nothing goes into `settings.ini`; no settings keys were added to `app/settings*.cpp`.

## Decisions

- Status rules (CDI-like): **Bad** = ATA SMART RETURN STATUS "threshold exceeded", any attribute with threshold
  1..0xFD and current <= threshold, NVMe critical warning read-only / reliability degraded / spare below threshold /
  backup device failed. **Caution** = raw 05 / C5 / C6 >= 1, NVMe media errors >= 1, remaining life <= 10 %, NVMe
  temperature warning bit (fixed CrystalDiskInfo defaults; per-drive thresholds: Removed 2026-11-11 (trim)). Every reason is listed in the window and the report.
- Life left: NVMe `100 - Percentage Used`; ATA the vendor's life attribute (normalized value; Samsung B1, Crucial CA,
  Intel E9, WD / SanDisk A9, Phison E7, generic SSD E7 / A9 / E9) or Device Statistics "percentage used".
- Self-tests only start from a button plus a confirmation dialog; buttons are disabled without admin. Progress is
  polled every 3 s while the window shows a running test. Abort is offered while one runs.
- Removed 2026-11-11 (trim). AAM / APM: implemented (SET FEATURES 0x42 / 0xC2, 0x05 / 0x85), behind a confirmation dialog, disabled without
  admin or on transports that can't send SET FEATURES (legacy SMART IOCTL). **Never applied on this machine** (no ATA
  drive anyway). Not reapplied at boot: that needs resident mode (§0).
- Removed 2026-11-11 (trim). History: a sample on every scan (window open / Refresh / CLI `--health`) when the last one is >= 1 h old or a wear /
  error counter (life used, 05, C5, C6, media errors) changed. Projection = first vs last sample with the value, needs
  >= 1 day: GB written per day (+ TB per year) and the date life used reaches 100 % when it has moved.
- USB: SAT first for most bridges (ASMedia, Realtek, VIA, Genesys, JMicron JMS56x/57x speak it); vendor CDBs only for
  their own VID (JMicron 0x152D, Prolific 0x067B, Sunplus 0x04FC, Cypress 0x04B4) so unknown bridges never get odd
  CDBs. NVMe bridges by VID:PID (152D:0583/0562/0586, 174C:2362/2364, 0BDA:9210/9211/9220), SAT fallback.
- RAID (CSMI) members are listed as extra drives ("RAID member (CSMI)"); the CSMI handles stay open for self-tests
  until the next scan / `disk_health_close`.
- Report (Copy / Save as text / CLI): CDI-like sections per drive: info, SMART table (raw MSB first, [CAUTION] /
  [BAD] tags), NVMe controller details (power states, features, firmware slots, error log), self-test log. CRLF,
  saved with a UTF-8 BOM. "Hide serial" masks serials in the window and the report.

## Measured facts (this laptop, not elevated)

- Samsung MZAL81T0HFLB-00BL2 (NVMe 2.0, PCIe 4.0 x4 current = max, DRAM-less with 64 MiB HMB preferred / 16 MiB
  min, 3 firmware slots, slot 2 active, 5 power states PS3/PS4 non-operational with 9 / 44 ms exit latency, WCTEMP 83 °C,
  CCTEMP 85 °C, extended self-test 35 min, host thermal management TMT1 81 °C / TMT2 83 °C, APST reported disabled by
  Get Features, two extra temperature sensors).
- Without admin, on a query-only handle (`CreateFile` access 0) stornvme answers: Identify Controller and Namespace,
  log pages 1 (error), 2 (health), 3 (firmware slots), 6 (self-test), Get Features 01/02/04/06/08/0C/10, temperature
  property (3 sensors), TRIM / write cache / seek penalty properties and `IOCTL_DISK_GET_DRIVE_LAYOUT_EX`. Refused:
  log 5 (commands supported), vendor logs C0 / CA, features 05 / 07 / 0D (HMB). Opening with GENERIC_READ or
  read/write fails with ERROR_ACCESS_DENIED, so pass-through IOCTLs, SMART IOCTLs and self-tests all need admin.
- Device Self-test (opcode 14h) is allowed through `IOCTL_STORAGE_PROTOCOL_COMMAND` (Microsoft's StorNVMe command
  table, Windows 10 1903+), which needs a read/write handle.

## FUTURE §1 status

| Item | Status |
|---|---|
| P1 ATA SMART table + vendor names | Done. Untested on real hardware (no SATA here); parsing / decoding / status / report checked with a synthetic Samsung 860 EVO IDENTIFY + SMART sector. |
| P1 Good / Caution / Bad, per-drive thresholds | Done (NVMe tested live, ATA synthetic). Per-drive thresholds: Removed 2026-11-11 (trim); fixed defaults. |
| P1 Drive info panel | Done: model, firmware, serial + hide toggle, interface, transfer mode current / max (PCIe from SetupAPI, SATA from IDENTIFY 76/77), standard, features, rotation / SSD, buffer, power-on count / hours, host reads / writes, NAND writes (ATA vendor attributes; NVMe drives don't report it in standard logs), partitions + letters + FS + labels. |
| P1 USB enclosures | Done, untested (no USB drives here). |
| P2 Self-tests | Done (ATA short / extended / conveyance / abort, NVMe short / extended / abort, progress + log). Not run: no admin in this session; the NVMe self-test log reads fine without admin. |
| P2 NVMe extras | Done: Identify Controller / Namespace summary + hex dumps, health log raw, error log, features, firmware slots, power states, temperature sensors 1–8, thermal transition counts / times. |
| P2 History + plots | Removed 2026-11-11 (trim) (was: per-field plot with hover, tested with a synthetic 30-day history). |
| P2 Endurance projection | Removed 2026-11-11 (trim) (was: from history). |
| P2 Alerts (toast / sound / email) | Skipped: needs resident mode and network (open FUTURE §0 decisions). |
| P3 AAM / APM | Removed 2026-11-11 (trim) (was: done, never applied here). |
| P3 RAID pass-through | CSMI for SATA members done, untested. Intel RST NVMe-in-RAID ("IntelNvm" miniport signature) and MegaRAID not done. |
| P3 Copy / save text report | Done (+ CLI `--health`). |

Untested paths (need hardware or admin): every ATA transport, SAT and vendor USB bridge CDBs, USB NVMe bridges,
CSMI, `SMART_RCV_DRIVE_DATA_EX` (undocumented; tried on query handles in case the driver allows it), ATA query-only
IDENTIFY / Device Statistics, NVMe self-test start / abort. JMicron "port 1" is found by retrying IDENTIFY,
not by reading the bridge register like smartmontools does.
