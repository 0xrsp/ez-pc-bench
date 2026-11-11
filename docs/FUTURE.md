# ez-pc-bench — Future (gap plan)

Goal: make ez-pc-bench a complete replacement for the usual Windows hardware toolbox — CrystalDiskInfo,
CrystalDiskMark, Cinebench, Unigine Heaven/Superposition, 3DMark, AIDA64, OCCT, HWiNFO, CPU-Z/GPU-Z,
Geekbench, HD Tune, Prime95/y-cruncher, MemTest-style testers, PresentMon/CapFrameX.
This file lists what those tools do that we don't (researched 2026-11-11). [PLAN.md](PLAN.md) stays the
source of truth for decisions and measured facts; move items there as they're designed and built.

Ground rules carried over (CLAUDE.md / PLAN.md): raw numbers with units stay the primary output; C-style
code; no new dependencies without discussing; keep the exes small and fast.

Priority tags: **P1** = needed to replace the tool in that column, **P2** = expected by power users,
**P3** = niche / stretch. **Later** = deliberately deferred (§14).

## What we already have (for reference)

CPU throughput kernels (int, FMA scalar/AVX2/AVX-512, SGEMM, SHA, AES, sort, sieve), core-to-core matrix,
thread scaling; DRAM bandwidth + latency + cache sweeps; GPU compute / 3D / 2D on Vulkan + D3D12 with
frame-time stats and live preview; CDM-style disk tests on several drives; NVMe health log + predict-failure
fallback; temperatures (ACPI zones, GPU via D3DKMT, drive sensor); sysinfo (CPUID, SMBIOS RAM, adapters,
drives, BitLocker, power); TOON results, compare window (up to 16 runs, summary), built-in reference
library, CLI.

## 0. Decisions

- [x] **Kernel driver → PawnIO, opt-in** (decided 2026-11-11). Use the existing signed PawnIO driver and
  its signed modules instead of writing or signing our own. Never WinRing0 / inpoutx64 / RTCore64 or
  other known-vulnerable drivers (Defender flags them, Microsoft's vulnerable-driver blocklist blocks
  them, and loading them looks like malware). Without PawnIO everything falls back to driverless sources.
  Details in §8.
- [x] **Overall score → yes, as an addition** (decided 2026-11-11). Raw numbers stay the primary output;
  the score is derived from them by a published, versioned formula and always shown with its breakdown.
  The "never invented scores" rule in CLAUDE.md / PLAN.md gets reworded when §10 is built. Details in §10.
- [x] **Stability testing → its own tool, separate from benchmarks** (decided 2026-11-11). Benchmarks
  measure speed; the stability tester looks for errors (CPU, memory error test, VRAM test, GPU). §6.
- [x] **Real rendering workloads → later** (decided 2026-11-11). The Cinebench-style CPU render and the
  Heaven-style heavy GPU scene are big projects; parked in §14.
- [ ] **Scope beyond this laptop.** "Replace everything" means other machines: Intel hybrid (P/E cores),
  multi-CCD Ryzen / X3D, >64 logical processors (processor groups), NUMA, non-AVX2 CPUs, Windows on ARM64.
  Decide which are in scope and get test hardware or volunteers.
- [ ] **Network features.** Update check, online result browser/leaderboard, email alerts all need
  network access. Decide what (if anything) talks to the network, always opt-in.
- [ ] **Resident mode.** CrystalDiskInfo/HWiNFO-style tray monitoring means a long-running process (and
  probably a scheduled task to start elevated at logon). Decide if the GUI exe may stay resident.

## 1. Disk health — CrystalDiskInfo parity

- [ ] **P1** Full ATA/SATA SMART table: ID, attribute name, current, worst, threshold, raw (hex + decoded),
  via `SMART_RCV_DRIVE_DATA` / `IOCTL_ATA_PASS_THROUGH`. Vendor attribute-name tables (Samsung, Crucial/
  Micron, WD/SanDisk, Kingston/Phison, Intel, Seagate, Toshiba...) as static arrays.
- [ ] **P1** Health status rules like CDI: Good / Caution / Bad from reallocated, pending, uncorrectable
  sectors, NVMe critical warning, spare below threshold, life used; thresholds user-adjustable per drive.
- [ ] **P1** Drive info panel: model, firmware, serial (with "hide serial" toggle for screenshots),
  interface and transfer mode **current vs max** (SATA/600, PCIe Gen x lanes for NVMe), standard (ATA8,
  NVMe 1.4...), features (SMART, TRIM, NCQ, volatile write cache, DevSleep, APM/AAM, 48-bit LBA),
  rotation rate / SSD, buffer size, power-on count/hours, host reads/writes, NAND writes when reported,
  partitions + drive letters.
- [ ] **P1** USB enclosures: SAT (SCSI-ATA translation) pass-through plus the common bridge quirks
  (JMicron, ASMedia, Realtek, Cypress); USB NVMe bridges (JMS583, RTL9210, ASM2362) vendor pass-through.
- [ ] **P2** Self-tests: ATA SMART short / extended / conveyance (`SMART_SEND_DRIVE_COMMAND`) and NVMe
  Device Self-test (admin 0x14 — check which Windows IOCTL allows it), with progress + self-test log.
- [ ] **P2** NVMe extras: Identify Controller/Namespace dump, error-information log, supported features
  (APST, HMB, namespaces, LBA formats), firmware slots, temperature sensors 1–8, thermal-management
  transition counts.
- [ ] **P2** History: persist health samples (temperature, life used, data written, reallocations) under
  `%APPDATA%\ez-pc-bench\health\` and plot them (CDI's SMART graph).
- [ ] **P2** Alerts (needs resident mode, §0): toast notification + optional sound on health change,
  temperature over threshold, new reallocated/pending sectors. Email alerts only if network is approved.
- [ ] **P2** Endurance projection: data written per day from history; life-used trend → estimated date
  to 100 % (no vendor TBW database needed).
- [ ] **P3** AAM / APM control for HDDs (presets Quiet…Performance, reapply at boot).
- [ ] **P3** RAID controllers: Intel RST / AMD RAIDXpert / MegaRAID pass-through (CSMI / vendor IOCTLs).
- [ ] **P3** Copy/save a text report of every drive (CDI "Copy" / "Save as text").

## 2. Disk benchmark — CrystalDiskMark, AS SSD, HD Tune, ATTO

- [ ] **P1** CDM profiles: Default, NVMe SSD, **Peak** (max QD/threads), **Real World** (Q1T1), Demo;
  Read-only / Write-only / Read+Write modes.
- [ ] **P1** **Mixed** read/write test (CDM Mix, default 70/30) and user-defined test rows (block size,
  queue depth, threads, read/write/mix ratio, random/sequential).
- [ ] **P1** Test data choice: random (incompressible) vs all 0x00 (shows controller compression).
- [ ] **P1** **Sustained write / SLC cache test**: write a large span (e.g. 50–90 % of free space, capped,
  user-confirmed) and plot MB/s vs GB written → SLC cache size, post-cache speed, folding behaviour; record
  drive temperature alongside to show thermal throttling.
- [ ] **P2** Queue-depth sweep chart (IOPS + latency vs QD 1…256) and block-size sweep (512 B…64 MiB,
  ATTO-style).
- [ ] **P2** Full-span read curve (HD Tune "Benchmark"): raw read across the whole device (read-only,
  admin) → transfer rate by LBA, access time, burst rate. Essential for HDDs.
- [ ] **P2** Surface / error scan (HD Tune "Error Scan"): read every block, map slow and unreadable
  sectors. Read-only.
- [ ] **P2** Real-world file workloads (AS SSD "Copy"): copy a large ISO-like file, a "program" set
  (many small files), a "game" set (mixed) through the normal file system; files/s and MB/s.
- [ ] **P2** Targets beyond drive letters: any folder (network share, mounted VHD, USB stick), with
  buffered vs unbuffered option.
- [ ] **P2** Unit switch: MB/s, GB/s, IOPS, µs; interval between tests (CDM "Interval") to let the drive cool.
- [ ] **P3** Compressibility sweep (AS SSD compression benchmark: 0–100 % compressible).
- [ ] **P3** TRIM/idle recovery test: write-heavy phase, idle N s, re-test.

### 2.1 Disk latency

CDM and AS SSD only show an average latency (or none). Latency, especially the tail, is what makes a PC
feel slow, and it's the most indicative disk number for the overall score (§10).
- [ ] **P1** Per-I/O timing (QPC at submit and completion) in every disk test into a fixed log-bucket
  histogram (no allocation in the I/O loop): p50 / p90 / p99 / p99.9 / p99.99 / max, plus the histogram
  as a series result.
- [ ] **P1** Dedicated **QD1 latency** tests: 4K random read, 4K random write, and 4K write + flush
  (`FlushFileBuffers` / FUA) — the cost of an fsync, which databases, browsers and installers pay.
- [ ] **P1** **Latency under load**: QD1 4K random read latency while other threads run sequential writes
  (and a variant with mixed random writes) — shows how badly background writes hurt responsiveness.
- [ ] **P2** Latency over time: per-100 ms p99 / max during sustained tests (§2 SLC test) → spikes from
  garbage collection, SLC folding and thermal throttling.
- [ ] **P2** Wake-up latency: idle the drive for 1 / 5 / 30 s, then time the first read (NVMe APST /
  power-state exit latency; large on laptops). Report alongside the drive's advertised exit latencies
  from Identify Controller.
- [ ] **P2** File-system metadata latency: create / open / stat / delete of small files (µs per op and
  ops/s), the cost behind app launches, builds and `git status`.
- [ ] **P3** HDD seek profile: access time vs seek distance.

## 3. CPU — Geekbench, y-cruncher, Prime95, CPU-Z bench

(The Cinebench-style render is in §14.)
- [ ] **P1** **Sustained mode** (Cinebench "minimum test duration"): loop any test for N minutes, report
  first pass, last pass, min, median and the drop in % (thermal / power-limit decay) with a per-pass chart.
- [ ] **P1** Per-core single-thread pass: run an ST kernel pinned to each core in turn → table/chart of
  per-core performance (preferred cores, CCD differences, P vs E cores). Pairs with the C2C matrix.
- [ ] **P1** Hybrid CPUs: report P-core and E-core results separately (EfficiencyClass from CPU sets).
- [ ] **P2** More real-world kernels, all reporting MB/s or items/s and self-verifying (important for
  the score, §10, which should lean on workload-like tests rather than peak-FLOPS microbenchmarks):
  - compression / decompression via the Windows Compression API (XPRESS, XPRESS-Huffman, MSZIP, LZMS —
    already used for the library, no new dependency) plus our own LZ77 for a pure-integer workload;
  - image decode/resize via WIC (JPEG/PNG decode MPix/s, built into Windows);
  - text/JSON parsing and tokenizing (branchy, Geekbench-"developer"-like);
  - FFT (complex radix-2/4, FP64), n-body physics, ray-box/ray-triangle;
  - CRC32C, xxHash-style hashing, AES-GCM / ChaCha20;
  - int8 dot products (AVX-VNNI / AVX-512 VNNI) and BF16 (AVX-512 BF16) for "AI inference" throughput.
- [ ] **P2** Big-number / π workload (y-cruncher-like): N digits of π via FFT multiplication; time and
  verified digits. Heavy on memory bandwidth + FP64.
- [ ] **P3** Instruction latency/throughput table (InstLatX64 / AIDA "CPU instruction latency"): selected
  instructions, clk/instruction.
- [ ] **P3** Branch predictor and uop-cache stress microbenchmarks (chips-and-cheese style).

## 4. Memory — AIDA64 cache & memory, Intel MLC

(The memory error test moved to the stability tester, §6.)
- [ ] **P1** AIDA-style **cache & memory grid**: read / write / copy / latency for L1, L2, L3 and DRAM in
  one table (we have read sweeps and latency; add write + copy per level and the summary grid).
- [ ] **P2** Loaded latency (Intel MLC): latency measured while other threads generate increasing
  bandwidth → latency vs bandwidth curve.
- [ ] **P2** Read/write mix ratios (1:1, 2:1, 3:1) and per-thread-count bandwidth scaling.
- [ ] **P2** Page-size effects: 4 KiB vs 2 MiB pages side by side when large pages are available.
- [ ] **P2** (PawnIO, §8) SPD and current timings shown in sysinfo and stored with every result.

## 5. GPU — 3DMark feature tests, GPU-Z

(The heavy Heaven-style scene is in §14; VRAM test and GPU error detection moved to §6.)
- [ ] **P2** **On-screen mode with a real swapchain**: fullscreen/windowed, vsync off with tearing,
  flip-model present; measure present-to-present and (where possible) displayed-frame times. Needed for
  Optimus/cross-adapter and VRR realism (offscreen is kept for clean throughput).
- [ ] **P2** Feature tests (3DMark-style, raw fps or rays/s):
  - ray tracing: DXR 1.1 inline + Vulkan ray query; Grays/s, BVH build/refit time;
  - mesh shaders vs classic vertex pipeline (same culled scene, both passes);
  - tessellation (hull/domain), variable-rate shading, sampler feedback (D3D12);
  - async compute overlap (graphics + compute queue concurrency gain in %).
- [ ] **P2** GPU memory hierarchy: pointer-chase latency and bandwidth sweep over L1/L2/(Infinity Cache)/
  VRAM — same charts as the CPU memory suite.
- [ ] **P2** More compute: FP64, INT8 dot4 (dp4a), FP16/INT8 matrix via cooperative matrix
  (`VK_KHR_cooperative_matrix`) / D3D12 equivalent when available; atomics throughput.
- [ ] **P2** Pipeline / shader compile time (PSO creation per second, cold vs cached) — relevant to stutter.
- [ ] **P2** Run on several adapters in one run (iGPU + dGPU), results keyed by adapter.
- [ ] **P3** Texture format sampling rates (BC1–BC7, R32F), anisotropic filtering cost.
- [ ] **P3** Frame capture of **other applications** (PresentMon/CapFrameX replacement) via ETW present
  events (Win32, no extra dependency): fps, frame times, 1 %/0.1 % lows of any running game, saved as TOON
  and comparable in the compare window.

## 6. Stability tester — separate tool (OCCT, Prime95, AIDA64 SST, MemTest86/TestMem5, memtestG80)

Its own tool window (like Temperatures / Disk health: a button on the System row, `--tool stability`)
and its own CLI mode (`--stress <tests> --minutes N`). It doesn't share the benchmark run list or
results table: benchmarks answer "how fast", this answers "does it make errors". Stability results are
saved as TOON with a `kind: stability` block so the compare window can ignore or show them.

Tests (tick any combination, run together or in sequence, duration or until stopped):
- [ ] **P1** **CPU**: several self-checking kernels (integer, SSE/AVX2/AVX-512 FMA with known answers,
  small data that stays in cache for max heat, large data that streams through memory — Prime95's
  small/large FFT idea); every result verified; errors attributed to the logical core that made them.
- [ ] **P1** **Memory error test**: fill as much free RAM as Windows allows (locked, pre-faulted) with
  patterns — moving inversions, walking 1s/0s, random, address-in-address, TestMem5-like mixes — on all
  threads; report errors with address, pattern, expected and actual bits; time or cycle count. State
  clearly that in-Windows coverage is partial (MemTest86 boots bare-metal and reaches all RAM).
- [ ] **P1** **VRAM test**: allocate almost all VRAM in chunks, pattern-test each (write, read back,
  compare on the GPU, count mismatches per chunk), on the selected adapter or every adapter.
- [ ] **P1** **GPU error detection**: compute and render workloads with known answers checked every
  iteration; count mismatches (unstable overclock / undervolt / VRAM).
- [ ] **P1** **Combined CPU + GPU ("PSU") load** for power delivery and cooling.
- [ ] **P1** **WHEA monitoring**: subscribe to Microsoft-Windows-WHEA-Logger events (`EvtSubscribe`) and
  count corrected/uncorrected hardware errors during the run (also shown during benchmark runs).
- [ ] **P1** Live sensor graphs (§7, §8) and safety limits: stop at a CPU / GPU / drive temperature the
  user sets; optional stop on first error; always restore state.
- [ ] **P2** Per-core cycling (CoreCycler): one core at a time under a heavy ST load for X minutes each —
  the Curve Optimizer / undervolt tuning use case.
- [ ] **P2** Cache test (L1/L2/L3-sized data, all cores) and a disk verify test (write, read back,
  checksum — catches bad cables, controllers, USB bridges).
- [ ] **P2** Report: pass/fail per test, time to first error, error counts per core / chunk, WHEA count,
  max temperatures, average clocks and power.
- [ ] **P3** Max-power GPU load ("power virus") for thermals/PSU, clearly labelled, with temperature cut-off.

## 7. Live monitoring — HWiNFO, HWMonitor, Task Manager

Driverless sources (always available; §8 adds the rest when PawnIO is installed):
- [x] **P1** CPU per-core **effective clock** without a driver: PDH "Processor Information\% Processor
  Performance" × base frequency (and "Processor Frequency"); per-core utilization; current power plan.
  *Done (M7): Sensors window, `core/hwread.*`. Utilization comes from `SystemProcessorPerformanceInformation`.*
- [x] **P1** **Record sensors during every benchmark**: sample clocks, temperatures, GPU clock/power/
  util in the background and store per-test avg/max with the results (clock, max temp, "throttled?").
  Makes every number explainable and comparable — no other benchmark does this well.
  *Done (M7): `core/monitor.*`, `env[]` table in TOON, a line under each result. CPU temperature waits for §8.*
- [x] **P1** GPU sensors (GPU-Z): core/memory clock, utilization per engine (PDH "GPU Engine"), VRAM used,
  power, fan, temperature; PCIe link speed/width current vs max (SetupAPI `DEVPKEY_PciDevice_*`, no driver).
  *Done (M7). The NVIDIA laptop driver reports a core clock only while the GPU is awake.*
- [x] **P1** Throttling indicators: effective clock vs max boost, power source changes, GPU perf-cap
  reason where the API exposes it. *Done (M7): clock drop ≥ 10 % within a phase, power source changes,
  hot GPU. No driverless API exposes the GPU perf-cap reason (that's NVML / ADLX), so it's left out.*
- [ ] **P2** Battery: charge/discharge rate in W, voltage, design vs full-charge capacity (wear), cycle
  count (`IOCTL_BATTERY_QUERY_STATUS` / `_INFORMATION`) — doubles as a system power meter on laptops.
- [ ] **P2** Energy Metering Interface (`IOCTL_EMI_*`) power rails where the platform exposes them.
- [ ] **P2** Sensor log to CSV/TOON at a chosen interval; min / max / avg columns with reset; per-sensor
  graphs; pick sensors to show.
- [ ] **P2** Tray icon with a chosen value (CPU or drive temperature); alerts on thresholds.
- [ ] **P3** Overlay: small top-most transparent window with chosen sensors and fps (no injection into games).

## 8. Advanced sensors via PawnIO (opt-in)

PawnIO (pawnio.eu, by namazso) is a signed kernel driver that only runs small signed modules (written in
Pawn) exposing narrow IOCTLs: MSR access, AMD SMN / SMU mailbox, SMBus controllers, Super I/O. It replaced
WinRing0 in LibreHardwareMonitor, FanControl, OpenRGB, ZenTimings and SMUDebugTool in 2025. It provides
register access, not sensors: the per-CPU-family decoding is ours (LibreHardwareMonitor, MPL-2.0, and
ZenTimings are good references for register addresses).

- [ ] **P1** Before building: confirm the licenses (client library LGPL-2.1+; check the PawnIO and
  PawnIO.Modules licenses for using / shipping the signed module `.bin` files), HVCI (memory integrity)
  compatibility, and that common anti-cheats don't object. Record the findings in PLAN.md.
- [ ] **P1** Integration: detect the PawnIO device; if missing, Settings shows "Advanced sensors: off"
  with a link to the official installer (we don't install drivers ourselves). Talk to the driver with
  plain `DeviceIoControl` from C (load module, call function) — no library dependency. Everything works
  without it; sensors just show "needs PawnIO".
- [ ] **P1** CPU, AMD Zen: Tctl/Tdie and per-CCD temperatures (SMN), per-core temperatures and clocks
  from the SMU PM table where the family exposes them, package power (RAPL MSRs), per-core effective
  clock (APERF/MPERF), core voltage (SVI telemetry).
- [ ] **P1** CPU, Intel: per-core and package temperature (IA32_THERM_STATUS / PACKAGE_THERM_STATUS),
  package / core / uncore power (RAPL), per-core effective clock, throttling reasons (thermal, power
  limit, PROCHOT) from the perf-limit-reason MSRs.
- [ ] **P1** RAM: DDR5 module temperature from the SPD hub's sensor (SPD5118) and DDR4 thermal sensors
  over SMBus (PIIX4 on AMD, I801 on Intel); SPD contents (manufacturer, part, die, ranks, JEDEC +
  XMP/EXPO profiles). On laptops the embedded controller may lock SMBus — handle "not available".
- [ ] **P2** Memory timings and clocks: AMD UMC registers (tCL, tRCD, tRP, tRAS, tRFC, ..., MCLK / FCLK /
  UCLK, gear/ratio) like ZenTimings; Intel from MCHBAR. Stored in sysinfo with every result.
- [ ] **P2** Desktop boards: fan RPM, voltages and board temperatures from Super I/O chips (Nuvoton,
  ITE, Fintek) via the LPC module.
- [ ] **P2** Polling rules: SMU / PM-table reads at most once per second (AMD's own guidance for its SDK);
  no SMBus or Super I/O polling during benchmark phases that it could disturb; one sampling thread.
- [ ] **P3** Clock stretching detection on AMD (effective vs requested clock under load).

## 9. System information — CPU-Z, GPU-Z, HWiNFO summary, Speccy

- [ ] **P1** CPU: codename, microcode revision (registry "Update Revision"), base/boost clocks,
  full ISA flag list, cache associativity, P/E core map.
- [ ] **P1** Motherboard / BIOS from SMBIOS types 0/1/2 (vendor, model, BIOS version and date), chipset
  from PCI IDs. *SMBIOS part done (M7); chipset still to do.*
- [ ] **P1** Perf-relevant Windows state, with warnings: VBS / HVCI (memory integrity), Game Mode,
  Hardware-accelerated GPU scheduling, Core Isolation, Secure Boot, TPM, Resizable BAR, active power
  overlay — these move benchmark numbers. *Done (M7, `core/sysinfo_win.cpp`) except Resizable BAR, which
  needs a D3D12 device (it wakes a sleeping laptop dGPU): read it in the GPU suite with the §9 GPU details.*
- [ ] **P1** GPU details (GPU-Z): shader/CU count, bus width, memory type/size, driver version + date,
  D3D feature level / shader model, Vulkan version + extension list, ReBAR, HAGS, VRR.
- [ ] **P2** Displays: EDID (model, native resolution, refresh range, HDR, VRR), connected adapter.
- [ ] **P2** PCI device tree with link generation/width (current vs max) for every device, not just the GPU.
- [ ] **P2** Export a full system report (TOON + plain text), copy to clipboard.
- [ ] **P3** Network adapters, audio devices, USB tree (Speccy completeness).

## 10. Overall score

One number for "how fast is this PC overall", plus one per component, computed only from the raw results
(so old `*.toon.txt` files get scores too) by a formula that is published in the app and the docs.

- [ ] **P1** **Normalization**: each input metric becomes a ratio vs a fixed reference machine (the
  library's reference run, this R7 250 + RTX 5060 Laptop, = 1000 per component). Lower-is-better units
  are inverted. The reference set is frozen per **score version**; changing inputs or weights bumps the
  version, and scores of different versions are never compared.
- [ ] **P1** **Components**, each a weighted geometric mean of its inputs:
  - *CPU single-thread* — workload-like ST tests (sort, sieve, SHA, AES, int ALU, compression and
    parsing once §3 adds them); peak FMA / AVX-512 with low weight.
  - *CPU multi-thread* — the same tests MT, plus SGEMM; thread-scaling efficiency as a minor input.
  - *Memory* — DRAM read / copy bandwidth and **latency** (latency weighted highest), L3 latency.
  - *GPU* — lit-scene frame time (p99 included, not just average), fill, texture fill, triangles, FP32,
    VRAM bandwidth; best of Vulkan / D3D12 per test so the API choice doesn't penalize.
  - *Storage* — dominated by **QD1 4K random read latency, latency under load and fsync latency (§2.1)**,
    then RND4K Q1T1, then sequential reads; sequential write and high-QD numbers count little (they
    barely affect how a PC feels).
- [ ] **P1** **Overall** = weighted geometric mean of the components. Weights express how much each
  component determines everyday performance. Ship profile weightings and show them all:
  - *Overall* (balanced): CPU ST 30 %, CPU MT 20 %, GPU 20 %, Memory 15 %, Storage 15 % (initial guess);
  - *Gaming*: GPU and CPU ST / memory latency dominate;
  - *Creator / workstation*: CPU MT, memory bandwidth, GPU compute;
  - *Everyday / office*: CPU ST and storage latency dominate.
- [ ] **P1** **How indicative is it** — calibration before the weights are trusted: collect reference
  machines' timings of real tasks (game fps from built-in game benchmarks, code compile time, app launch,
  photo export, file copy — published reviews or volunteers) and fit / sanity-check the weights so the
  score ranks those machines in the same order. Record the method, data and correlation in PLAN.md; show
  the score version's correlation in the app's "About the score" page. Until calibrated, label it
  "preliminary".
- [ ] **P1** **Validity rules**: a score needs the required tests at default settings; partial runs get
  component scores only. Flag (don't hide) runs on battery, non-best power mode, thermal throttling
  detected (§7), background load, HVCI changes — shown next to the score.
- [ ] **P1** UI: overall score + component bars with their raw inputs on hover/expand, "vs reference"
  and vs library machines (§11 ranking), and the profile scores. Stored in TOON as derived values with
  the score version.
- [ ] **P2** CLI `--score` prints the breakdown; compare window gets a score row per run.
- [ ] **P2** When the §14 workloads land, add them as inputs in a new score version.

## 11. Results, history, comparison, sharing

- [ ] **P1** **Ranking view** (Cinebench ranking list): per test and for the score (§10), a bar chart of
  this run against the library's reference runs. Grow the library with many machines.
- [ ] **P1** Run presets: Quick (~1 min), Standard, Extended, Sustained/thermal, Score (exactly the
  tests the score needs) — one click. *Quick / Standard / Extended done (M7, `core/presets.*`, GUI combo,
  CLI `--preset`); Sustained comes with §3 sustained mode, Score with §10.*
- [x] **P1** Repeat N times: median, min, max and spread (CV %) per metric; flag noisy metrics.
  *Done (M7): `repeat` setting / `--repeat`, `spread[]` table in TOON, values with CV ≥ 3 % marked.*
- [ ] **P2** Automatic run history (every run saved under `results\history\`) and a trend chart per
  metric across runs (driver/BIOS/Windows update regressions).
- [ ] **P2** Shareable output: render the results/compare table to PNG (CDM-style screenshot) and a
  self-contained HTML report.
- [ ] **P2** Community results without a server: submit `*.toon.txt` via a GitHub PR / issue form;
  the website renders a searchable table from the library (static, built by the Pages workflow).
- [ ] **P3** Result integrity: hash of exe + system block in the file to spot edited results (weak, but
  something).

## 12. App / platform

- [ ] **P1** Hardware generality (decide in §0): processor groups (>64 LPs), Intel hybrid, multi-CCD
  topology display, NUMA-aware memory tests, graceful AVX2-less path.
- [ ] **P2** Opt-in update check against GitHub Releases.
- [ ] **P2** Start-with-Windows resident mode (scheduled task for elevation) for health/temperature alerts.
- [ ] **P2** Keyboard navigation and screen-reader friendly labels in the main lists.
- [ ] **P3** Windows on ARM64 build (NEON kernels; Cinebench and Geekbench run natively there).
- [ ] **P3** Localization (string table), starting with the strings in tool windows.

## 13. Suggested order

1. **M7 Context** — §7 recording sensors during runs, §9 perf-relevant Windows state warnings, §11
   presets + repeat/median. Cheap, and it makes every existing number more trustworthy.
2. **M8 Disk** — §2.1 latency (histograms, QD1, latency under load, fsync), §1 ATA SMART table, health
   rules, drive info panel, USB bridges; §2 profiles, mix, custom rows, sustained-write/SLC test.
3. **M9 Stability tester** — §6 tool window, CPU kernels with per-core errors, memory error test, VRAM
   test, GPU error detection, WHEA, temperature limits.
4. **M10 PawnIO sensors** — §8 license/HVCI check, integration, AMD then Intel CPU sensors, RAM
   temperature + SPD, timings. Feeds §6 and §7.
5. **M11 Score** — §3 workload-like CPU kernels first (better score inputs), then §10 formula,
   calibration, UI, ranking (§11).
6. **M12 Breadth** — §5 feature tests and GPU memory hierarchy, §2 full-span/surface scan, §4 grid +
   loaded latency, §11 history + sharing, §1 self-tests + history + alerts.
7. **Later** — §14.

## 14. Later (not yet): real rendering workloads

Big projects with their own art/engine work; start only after M7–M12.
- [ ] **Later** **CPU render (Cinebench-like)**: own path tracer with BVH, an original procedural scene,
  fixed sample count; ST and MT; Msamples/s and render time; the image builds up bucket by bucket in the
  existing preview pane; works with the sustained mode (§3) for a "minimum test duration" loop.
- [ ] **Later** **Heavy GPU scene (Heaven / Superposition-like)**, original procedural content (no copied
  assets or scenes): deterministic camera path, PBR lighting, shadows, tessellation, post-processing,
  many draw calls; presets Low / Medium / High / Extreme, resolutions 720p…4K/8K + custom; avg / p1 /
  p0.1 frame times and a frame-time graph; uses the on-screen mode (§5).
- [ ] **Later** **Loop / stability mode on the heavy scene** (3DMark stress test, Heaven loop): N minutes
  or 20 loops; frame-rate stability (worst loop / best loop %), clocks, temperature, power over time;
  also offered in the stability tester (§6).
- [ ] **Later** Add both workloads to the score (§10) in a new score version.

## Sources (2026-11-11)

- CrystalDiskInfo features (SMART table, alerts, AAM/APM, graphs, USB): [Crystal Dew World](https://crystalmark.info/en/?p=118),
  [alert setup guide](https://mundobytes.com/en/configure-alerts-in-crystaldiskinfo)
- CrystalDiskMark profiles / Mix / read-only modes: [CDM 8.0.0 notes](https://crystalmark.info/en/2020/11/20/crystaldiskmark-8-0-0/),
  [changelog](https://www.softpedia.com/progChangelog/CrystalDiskMark-Changelog-70870.html)
- Cinebench 2024 (ST/MT/GPU, minimum test duration): [Maxon tech info](https://maxon.net/tech-info-cinebench),
  [Puget Systems](https://www.pugetsystems.com/labs/articles/cinebench-2024-redefining-rendering-performance-evaluation/)
- Unigine Heaven / Superposition (presets, tessellation, loop/stress): [Heaven manual](https://benchmark.unigine.com/docs/heaven/User_Manual.pdf),
  [Superposition manual](https://benchmark.unigine.com/docs/superposition/Superposition_Benchmark_User_Manual.pdf)
- 3DMark feature tests and stress tests: [mesh shader test](https://benchmarks.ul.com/en/news/new-3dmark-test-measures-mesh-shader-performance),
  [ray-tracing test](https://benchmarks.ul.com/en/news/new-3dmark-test-measures-pure-raytracing-performance),
  [3DMark](https://benchmarks.ul.com/3dmark)
- AIDA64 (cache & memory, GPGPU, stability test): [benchmarks](https://www.aida64.com/user-manual/benchmarks),
  [stability test](https://www.aida64.com/user-manual/tools/stability-test)
- OCCT (stability, VRAM, PSU, WHEA, error detection): [OCCT features](https://www.ocbase.com/features)
- HWiNFO (sensors, logging, effective clock, throttling): [PCGH review](https://www.pcgameshardware.de/Benchmarks-Thema-58180/Specials/HWiNFO-Pro-Download-Portable-Test-1374622/);
  shared-memory 12-hour limit in the free version: [HWiNFO forum](https://hwinfo.com/forum/threads/important-changes-to-hwinfo64-coming-soon.7092/page-5)
- Geekbench 6 workloads: [CPU workloads](https://Geekbench.com/doc/geekbench6-cpu-workloads.pdf),
  [GPU compute workloads](https://Geekbench.com/doc/geekbench6-gpu-compute-workloads.pdf)
- GPU-Z (sensors, PCIe render test, BIOS save): [TechPowerUp](https://techpowerup.com/gpuz)
- PawnIO (signed modules, WinRing0 replacement, adoption): [OpenRGB merge request](https://gitlab.com/CalcProgrammer1/OpenRGB/-/merge_requests/2833),
  [OpenRGB commit](https://gitea.marinkevich.ru/grayhook/OpenRGB/commit/30a250c7df4b38380b24eacd871eaa75d125b9b3),
  [ZenTimings](https://github.com/irusanov/ZenTimings/discussions/90),
  [FanControl V238](https://newreleases.io/project/github/Rem0o/FanControl.Releases/release/V238),
  [SidebarDiagnostics](https://newreleases.io/project/github/thewriteway/SidebarDiagnostics/release/v3.7.4),
  [OCCT announcement](https://ocbase.com/news/winring-driver-alternative-announcement),
  [nwinfo driver notes](https://deepwiki.com/a1ive/nwinfo/6.2-kernel-drivers-and-low-level-hardware-access)
- AMD Ryzen Master Monitoring SDK (per-core temperature/frequency, SMU polling ≤ 1 Hz): [AMD](https://www.amd.com/en/developer/ryzen-master-monitoring-sdk.html)
