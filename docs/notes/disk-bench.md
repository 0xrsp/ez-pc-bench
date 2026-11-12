# Disk benchmark stream (FUTURE.md §2, §2.1)

What was built for the disk suite on top of the CrystalDiskMark-style M3 tests, how it is wired, and what was
measured. Status per FUTURE item at the end.

**Removed 2026-11-11 (trim):** the custom tests `disk.custom1`-`disk.custom4` (rows, settings, `--disk-custom`, TOON
`disk_custom`), the CDM disk profiles (GUI combo, `--disk-profile`) and the test-data option (`disk_data`,
`--disk-data`; every test now writes random, incompressible data) were removed. The compressibility sweep stays.
Settings files may still hold `disk_data` / `disk_custom1..4`; they are ignored. Parts below that describe the
removed features are marked.

## Files

`src/disk/disk_bench.cpp` was split:

| File | Contents |
|---|---|
| `disk_internal.h` | Shared types: `DiskTarget` / `DiskSuite`, latency histogram (`LatHist`), I/O engine (`IoSpec`, `IoRun`, `IoResult`), metric helpers |
| `disk.h` | Public helpers for GUI / CLI: mode names, display units, confirm notes, TOON params (CDM profiles, custom-row text format: Removed 2026-11-11 (trim)) |
| `disk_suite.cpp` | Suite init / shutdown, test file per drive or in a folder, work folder, name interning (`dname`), temperature, interval |
| `disk_io.cpp` | Histogram + percentiles, the overlapped I/O engine, `add_io_metrics` |
| `disk_cdm.cpp` | CDM rows (Default / NVMe), units, confirm notes, TOON params (custom rows, profiles, row parser: Removed 2026-11-11 (trim)) |
| `disk_latency.cpp` | QD1 read / write / write+flush / FUA, latency distribution series, latency under load, wake-up latency |
| `disk_nvme.cpp/.h` | NVMe Identify Controller power states (entry / exit latencies) |
| `disk_sweep.cpp` | Queue-depth, block-size and compressibility sweeps |
| `disk_sustained.cpp` | Sustained write / SLC cache with latency over time and drive temperature |
| `disk_recovery.cpp` | TRIM / idle recovery |
| `disk_files.cpp` | File-system metadata latency, ISO / program / game copy sets |
| `disk_raw.cpp` | Raw device: full-span read curve, surface scan, seek profile (admin, read-only) |

## Engine and latency histograms (§2.1 P1)

- Every file-based disk test goes through `io_run`: per thread one unbuffered overlapped handle, one completion port,
  `qd` requests in flight (1..256), resubmitted on completion. Several groups can run at once (a QD1 reader next to
  background writers). Groups can be time-bound or bounded by a request count (whole-file passes).
- Each request is timed with QPC from submit to completion (completion time = the `GetQueuedCompletionStatusEx`
  return, shared by a batch) and added to a per-thread `LatHist`: exact below 32 ticks, then 16 log-linear buckets
  per power of two (~4 % resolution), 592 buckets, values up to 2^40 ticks. Adding is a bit scan and three adds;
  nothing is allocated in the I/O loop (histograms live in the static worker array).
- Percentiles walk the merged histogram and report the bucket midpoint (clamped to the exact max): p50 / p90 / p99 /
  p99.9 / p99.99 / max. QPC runs at 10 MHz here, so latencies are 100 ns-granular.
- Read / write / mixed I/Os are kept in separate histograms.

## Tests (ids, registry order = run order)

Default selection gained only cheap tests (marked *): QD1 latency (reads, ~200 MB of 4K writes, flush) and metadata.

| Id | Kind | What |
|---|---|---|
| `disk.seq1m_q8t1`, `seq1m_q1t1`, `seq128k_q32t1`, `rnd4k_q32t1`, `rnd4k_q1t1`, `rnd4k_q32t16` | values | CDM rows as before, now with `p50` .. `max` per pass type; passes follow the read / write mode |
| `disk.sweep.block` | series | ATTO-style: sequential QD4, 512 B .. 64 MiB, read and write MB/s |
| `disk.sweep.compress` | series | Whole-file SEQ1M Q8 write then read with 0/20/40/60/80/100 % zeros; restores the test data afterwards |
| `disk.sweep.qd` | series | RND4K IOPS, avg and p99 latency at QD 1..256 (one thread to QD32, then 32 per thread) |
| `disk.custom1..4` | values | Removed 2026-11-11 (trim). Was: user rows (skipped while empty); the row spec was part of the metric names |
| `disk.lat.read` *, `disk.lat.write` * | values | QD1 4K random, avg / percentiles / IOPS |
| `disk.lat.flush` * | values | QD1 4K write + `FlushFileBuffers` per write, and 4K write with FUA (`FILE_FLAG_WRITE_THROUGH`) |
| `disk.lat.hist` | series | QD1 4K read / write latency distribution: % of I/Os per bucket, 4 buckets per octave, fixed 61 points (≤ 3.2 µs .. ≥ 105 ms) |
| `disk.lat.load` | values | QD1 4K read latency idle, under SEQ1M Q8 writes and under RND4K Q32 writes (other half of the file) + background MB/s |
| `disk.lat.wake` | values | First 4K read after 1 s (×5), 5 s (×3), 30 s (×2) idle: median and max; active QD1 p50; NVMe PSn entry / exit latencies from Identify Controller |
| `disk.fs.meta` * | values | 500 small files per round: create+write 1 KiB, open+read, stat, delete — ops/s, avg, percentiles |
| `disk.files.copy` | values | ISO (1 file = test-file size), Program (many 4 KiB..4 MiB files, half the size), Game (256 KiB..32 MiB files) copies: MB/s, files/s |
| `disk.recovery` | values | TRIM + rewrite, fresh seq read / write, RND4K Q32T4 torture (3x duration), after torture, after 30 s idle, after TRIM + rewrite |
| `disk.sustained.write` | series | Sustained write / SLC cache: MB/s, p99 and max latency (worst 100 ms window), drive temperature vs GB written + summary points. `BENCH_CONFIRM` |
| `disk.raw.span` | series | Raw read MB/s at 100 positions (64 MiB each), plus min / avg / max, access time, burst rate |
| `disk.raw.scan` | values | Reads every sector in 4 MiB blocks; blocks per read-time class (<10, 10-50, 50-150, 150-500, >500 ms), read errors, slowest block. `BENCH_CONFIRM` |
| `disk.raw.seek` | series | Access time vs seek distance (100 % .. 1/32768 of the disk) |

Series results may hold several named lines (e.g. `C: Read`, `C: Write`) and one-point "summary" lines. The GUI's
Charts tab now draws one plot per line and shows one-point lines as text (`ui.cpp draw_series`).

## Settings, CLI, TOON

New `BenchParams::disk` (`DiskOptions`, core/bench.h): mode, mix read %, display unit, buffered, interval,
sustained cap, folder (test data and 4 custom rows: Removed 2026-11-11 (trim)).

| ini key (`[settings]`) | CLI | TOON `params:` |
|---|---|---|
| `disk_mode=rw\|read\|write\|mix` | `--disk-mode` | `disk_mode` |
| `disk_mix_read_pct=70` | `--disk-mix` | `disk_mix_read_pct` |
| `disk_data=random\|zero` | `--disk-data` | `disk_data` (Removed 2026-11-11 (trim)) |
| `disk_unit=MB/s\|GB/s\|IOPS\|us` | (GUI only) | - |
| `disk_buffered=0\|1` | `--disk-buffered` | `disk_buffered` |
| `disk_interval_ms` | `--disk-interval` | `disk_interval_ms` |
| `disk_sustained_gib=64` | `--sustained-gib` | `disk_sustained_gib` |
| `disk_folder=` | `--disk-folder` | `disk_folder` (only when set) |
| `disk_custom1..4=RND4K Q32T4 R70` | `--disk-custom "row;row"` | `disk_custom[N]` (Removed 2026-11-11 (trim)) |

Also: `--yes` (without it the CLI drops `BENCH_CONFIRM` tests with a note). GUI: a "folder" marker when a folder
target is set, Settings > Disk rows for all options, and a confirmation dialog before a run that includes
`BENCH_CONFIRM` tests. Removed 2026-11-11 (trim): `--disk-profile default|nvme|peak|realworld|demo`, the GUI profile combo after
the drive chips and the "Custom disk tests" settings group.

Custom row text (Removed 2026-11-11 (trim)): `<SEQ|RND><size>[K|M] Q<qd>T<threads> [R<read %>]`; spaces, commas or `_` separate the parts.
Without `R` the row runs read and write passes like the built-in rows; `R100` read only, `R0` write only, else one
mixed pass.

## Decisions

- **Percentiles everywhere, histogram series in one test.** A result has one kind, so the throughput tests stay
  `values` and gain p50..max per pass type; the full distribution is its own series test (`disk.lat.hist`). Its
  points are a fixed grid (61 per line), so repeats (median of N) and compares line up. The sustained-write
  series likewise uses fixed GB marks (100 points), not "one point per window".
- **Requests in flight at the end of a pass** are drained for up to 1 s and their latency is recorded (not counted
  for throughput). Before, they were cancelled and dropped, so a request stalled across the end of a pass vanished
  from the tail; only after 1 s are they cancelled.
- **Sustained write uses 4 files written in parallel**: extending writes are synchronous on NTFS, and
  `SetFileValidData` needs a privilege (and would expose stale disk contents). With 4 files the drive sees QD4 1 MiB:
  5–7 GB/s here, vs ~2.8 GB/s for one synchronous stream (SEQ1M Q1T1).
- **Heavy tests**: new flag `BENCH_CONFIRM` (sustained write, surface scan): the GUI asks first (dialog lists the
  tests and what they do), the CLI drops them unless `--yes`, presets (incl. Extended "every test") never select them.
- **The test file can't be deleted by another instance** any more: it is held without `FILE_SHARE_DELETE`. On
  current Windows `DeleteFileW` uses POSIX semantics and unlinks a file that is open with share-delete, so a second
  instance's stale-file cleanup removed `C:\ezbench\ezbench.tmp` and its folder under a running test (seen during
  verification: "cannot open test file (error 3)").
- Removed 2026-11-11 (trim). CDM profiles were test selections (like run presets), not a setting: the combo shows the profile the disk part of
  the tree matches. Peak = SEQ1M Q8T1 + RND4K Q32T16, Real World = SEQ1M Q1T1 + RND4K Q1T1, Demo = SEQ1M Q8T1.
- Display unit (MB/s / GB/s / IOPS / µs) only changes the GUI tree summary and MB/s -> GB/s in the results table;
  result files always hold every number.
- Raw-device tests open `\\.\PhysicalDriveN` with `GENERIC_READ` only (no write access at all); the engine opens a
  read-only handle for read-only specs, so no code path can write to the device.
- The folder target replaces the drive letters for the whole suite (metrics named `Folder: ...`); raw tests then use
  the physical disk of the folder's volume (none for UNC paths).

## Measured (2026-11-11, C: Samsung MZAL81T0HFLB 1 TB NVMe, AC, "Best performance", not elevated, 256 MiB file, 1 s phases)

Runs 4–6 were clean. Runs 1–3 overlapped another agent's disk test: the lock wrapper treats a lock older than
30 min as stale, so a long run lost its lock. Their throughput numbers are not usable. The latency tests below
come from the clean run 5 (median of 2).

| Test | Result |
|---|---|
| QD1 4K read (`disk.lat.read`) | avg 58.7 µs, p50 56, p90 70, p99 112, p99.99 461, max 484 µs; 17.0K IOPS |
| QD1 4K write | avg 21.2 µs, p50 17.2, p99 65.6 µs; 47.2K IOPS |
| 4K write + FlushFileBuffers | avg 544 µs, p50 525, p99 742 µs (1.8K IOPS): an fsync costs ~0.5 ms here |
| 4K write FUA (write-through) | avg 21 µs, same as a plain write: this drive / StorNVMe make FUA free, the flush is what costs |
| Histogram, 4K read QD1 | 84 % of reads within 51–64 µs buckets, 2.5 % > 100 µs, tail to ~0.7 ms |
| QD sweep, 4K random read | 15.4K IOPS QD1 → 109K QD8 → 133K QD16 → 130K QD32 (1 thread tops out) → 223K QD64 → 788K QD256 (8 threads) |
| QD sweep, 4K random write | 44K QD1 → 113K QD8 → ~106K QD32 → 630K QD256 |
| Block sweep, seq Q4 read | 25 MB/s @512 B, 288 @4K, 2.3 GB/s @64K, 6.5 GB/s @1M, peak 6.65 @2–8 MiB, 5.6 @32–64 MiB |
| Block sweep, seq Q4 write | 29 MB/s @512 B, 160 @4K, 3.4 GB/s @64K, ~5.2–5.3 GB/s @1–16 MiB, 2.5 / 1.7 GB/s @32 / 64 MiB |
| Compressibility | no effect: reads 6.95–7.06 GB/s at 0–100 % zeros; writes 4.6–7.0 GB/s, just noise (CV up to 38 % between 2 runs) |
| RND4K Q1T1 mix 70/30 | 82.8 MB/s, 20.2K IOPS, p50 54 µs (reads 16.6K / writes 47.8K IOPS separately) |
| Custom `RND4K Q8T2 R70` (Removed 2026-11-11 (trim)) | 904 MB/s, 221K IOPS, p50 56 µs, p99 150 µs |
| Latency under load (run 2, may be disturbed) | QD1 read idle p50 82 / p99 131 µs; next to SEQ1M Q8 writes (5.35 GB/s) p50 150 / p99 678 / p99.9 3789 µs; next to RND4K Q32 writes (268 MB/s) p50 69 / p99 182 µs |
| Wake-up (run 2, may be disturbed) | active p50 75 µs; after 1 s idle median 1.67 ms (max 10.1 ms), 5 s 0.20 ms, 30 s 0.26 ms. Identify Controller (no admin needed): PS3 exit 9 ms / entry 4.5 ms, PS4 exit 44 ms / entry 4.5 ms |
| Sustained write, 2 GiB | average 4974 MB/s, points 5–7 GB/s with dips to 1.4–2 GB/s, 53 °C throughout, no cache drop within 2 GiB (expected: the SLC cache is far larger) |
| TRIM / idle recovery | fresh seq read 5800 / write 2967 MB/s; RND4K Q32T4 torture 19.6K IOPS; after torture read **851** / write 2278; after 30 s idle 5464 / 3625; after rewrite read 6570 MB/s |
| Copy sets (256 MiB budget) | ISO 1008 MB/s; Program 188 MB/s, 465 files/s; Game 557 MB/s, 68.5 files/s |
| Metadata (folder on C:, run 3, may be disturbed) | create 302/s (p50 1.25 ms), open+read 781/s, stat 9662/s (p50 28 µs), delete 611/s. Defender's on-access scan is most of create / open |

Other facts:
- `FSCTL_FILE_LEVEL_TRIM` fails with error 326 (`ERROR_DEVICE_FEATURE_NOT_SUPPORTED`) on C:, buffered or not, so the
  recovery test reports `After rewrite (no TRIM) seq read`.
- Random-write torture on a small file cut sequential reads 7x (5.8 → 0.85 GB/s) and 30 s idle brought them back
  to 5.5 GB/s: the drive's background garbage collection, consistent with PLAN.md §13's "sequential tests first".
- Buffered folder target: 4K QD1 reads p50 7 µs (OS cache); 1 MiB QD1 reads 2.7 GB/s.
- GUI: the Settings window showed a Dear ImGui error ("Invalid flags for IsWindowHovered()") on dev; fixed (b8685a8).

## Status of the FUTURE.md items

§2:
- [x] P1 Read / Write / Read+Write (+Mix) modes. CDM profiles (Default, NVMe SSD, Peak, Real World, Demo): Removed 2026-11-11 (trim).
- [x] P1 Mixed test (mode "mix", 70/30 adjustable). 4 user-defined rows: Removed 2026-11-11 (trim).
- [ ] P1 Test data random vs 0x00: Removed 2026-11-11 (trim) (always random).
- [x] P1 Sustained write / SLC cache with drive temperature; verified at 2 GiB only (per the brief), so the cache-drop
  detection has not seen a real cache exhaustion yet.
- [x] P2 Queue-depth sweep (QD 1..256) and block-size sweep (512 B..64 MiB).
- [~] P2 Full-span read curve and [~] P2 surface scan: implemented, **untested on a real device** (need admin; the
  session wasn't elevated). The code paths ran once against a scratch file through a temporary hook (not committed).
- [x] P2 Real-world copy workloads (ISO / program / game).
- [x] P2 Any-folder target with buffered / unbuffered (verified on a local folder; not on a network share or USB stick).
- [x] P2 Unit switch and interval between tests.
- [x] P3 Compressibility sweep.
- [x] P3 TRIM / idle recovery (TRIM itself is refused on this C:, see above).

§2.1:
- [x] P1 Per-I/O QPC timing into a fixed log-bucket histogram in every engine-based disk test, p50..max, no allocation
  in the I/O loop. [~] The histogram as a series is a dedicated test (`disk.lat.hist`), not part of every result (one
  result kind per test). The surface scan uses read-time classes instead of percentiles.
- [x] P1 QD1 latency: 4K read, 4K write, 4K write + flush, plus FUA.
- [x] P1 Latency under load (sequential-write and random-write variants).
- [x] P2 Latency over time: worst 100 ms p99 / max per point in the sustained-write test.
- [x] P2 Wake-up latency (1 / 5 / 30 s) with the NVMe PSn entry / exit latencies (local Identify Controller helper,
  `disk_nvme.cpp`; physdisk / disk_health untouched).
- [x] P2 File-system metadata latency.
- [~] P3 HDD seek profile: implemented, untested on a device (admin; and this PC has no HDD).

Nothing in this stream needed an open FUTURE §0 decision (network shares are plain file paths; no network code).

## Shared-file edits (for merging)

- `core/bench.h`: `DiskOptions` / `DiskRow` / enums before `BenchParams`, `DiskOptions disk;` appended to
  `BenchParams`, flag `BENCH_CONFIRM = 1u << 4`.
- `core/registry.cpp`: disk declarations and rows (inserted in the disk block; existing ids and order unchanged).
- `core/presets.cpp`: presets never select `BENCH_CONFIRM` tests (one condition).
- `core/results.cpp`: `#include "disk/disk.h"`, `disk_write_toon_params` at the end of `write_params`.
- `CMakeLists.txt`: `src/disk/disk_bench.cpp` replaced by the ten `src/disk/*.cpp` files; no new libraries.
- `app/main_cli.cpp`, `app/settings.cpp`, `app/settings_ui.cpp`, `app/ui.cpp`: additive (see above).
