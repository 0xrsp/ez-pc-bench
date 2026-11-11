# ez-pc-bench

A small native Windows benchmark for CPU, memory, GPU (Vulkan and Direct3D 12) and disks. It reports raw
numbers (GFLOPS, GB/s, ns, IOPS, FPS) with explicit units. It comes as a GUI and a command-line exe
that run the same tests. Website: <https://0xrsp.github.io/ez-pc-bench/>.

![ez-pc-bench](docs/screenshot.png)

## Features

### Done

**Benchmarks**
- [x] **CPU:** integer and FP32/FP64 FMA throughput (scalar, AVX2, AVX-512), SGEMM, SHA-256, AES, sort,
  prime sieve, core-to-core latency matrix and thread scaling. Each runs single-threaded and on all
  threads, and every test with a checkable output verifies itself.
- [x] **Memory:** DRAM read, write, copy and triad bandwidth (normal and non-temporal), latency from
  4 KiB to 1 GiB, and cache bandwidth sweeps for one thread and all threads.
- [x] **GPU (Vulkan + D3D12):** both APIs run the same shaders, so their numbers compare directly
  - compute: FP32, FP16 and INT32 throughput, VRAM and shared-memory bandwidth, PCIe transfers
  - 3D: triangles, fill rate, texture fill, draw calls, a lit scene, with avg / p99 / p99.9 frame times
    (the GUI also shows them as fps) and a live preview
  - 2D: sprites, blit and clear
- [x] **Disk:** CrystalDiskMark-style SEQ1M and RND4K at several queue depths (plus NVMe extras),
  reads and writes, best of N passes, MB/s, IOPS and average latency, on one or more drives.

**Tools**
- [x] **Temperatures:** live CPU (ACPI zones), GPU (temperature, fan, power) and drive temperatures.
- [x] **Disk health:** NVMe health log (life used, spare blocks, data written, power-on hours, errors),
  SMART predict-failure on other drives.
- [x] **System info:** CPU (CPUID, caches, ISA), RAM modules (SMBIOS), GPUs, drives (bus, file system,
  VHD / Dev Drive, BitLocker), power source and plan, OS; warnings for anything that skews results.
- [x] **Results:** TOON text files, a compare window for up to 16 runs (summary, per-suite tabs,
  change vs a reference run, copy table), a built-in library of reference runs.
- [x] GUI and CLI with the same tests; light / dark theme; settings saved automatically.

### To do

The full plan, with priorities and sources, is in [docs/FUTURE.md](docs/FUTURE.md).

- [ ] **Disk latency:** p50 to p99.99 and max for every test, QD1 read / write / flush latency, latency
  under background writes, latency over time, wake-from-idle latency, small-file operations.
- [ ] **Disk benchmark:** CrystalDiskMark Peak / Real World profiles, mixed read/write, custom tests,
  sustained-write (SLC cache) test, queue-depth and block-size sweeps, full-drive read and surface scan.
- [ ] **Disk health (CrystalDiskInfo):** full SATA SMART table, Good / Caution / Bad rules, drive info
  panel, USB enclosures, self-tests, health history and alerts.
- [ ] **Stability tester (separate tool):** self-checking CPU stress with per-core error tracking,
  memory error test, VRAM test, GPU error detection, combined CPU + GPU load, Windows hardware-error
  (WHEA) logging, temperature limits.
- [ ] **Sensors:** clocks, temperatures and power recorded during every benchmark and saved with the
  results; per-core clocks, GPU clocks and PCIe link, battery power, sensor logging.
- [ ] **Advanced sensors (opt-in PawnIO driver):** per-core CPU temperatures and power, RAM
  temperatures, module details and memory timings, fans and voltages on desktop boards.
- [ ] **CPU / memory / GPU:** sustained (thermal) mode, per-core pass, P / E core results, more
  real-world workloads, AIDA64-style cache and memory grid, loaded latency, GPU ray tracing and
  mesh-shader tests, GPU memory hierarchy.
- [ ] **System info:** motherboard / BIOS, microcode, Windows settings that affect performance (VBS,
  HAGS, Game Mode, ReBAR), GPU details, displays, PCI link speeds.
- [ ] **Overall score:** derived from the raw results with a published, versioned formula; component
  scores for CPU, memory, GPU and storage; checked against real-task timings before it's trusted.
- [ ] **Results:** ranking against reference machines, one-click presets, repeat runs with medians,
  run history, PNG / HTML export.
- [ ] **Later:** a Cinebench-style CPU render and a Heaven-style heavy GPU scene.

## Download

Get `ez-pc-bench.exe` (GUI) and/or `ez-pc-bench-cli.exe` from
[Releases](https://github.com/0xrsp/ez-pc-bench/releases). You need Windows
10/11 x64, a CPU with AVX2, and a GPU driver with Vulkan support (it installs `vulkan-1.dll`). The GPU
tests need D3D12 and Vulkan 1.3.

## Build

Requirements:
- LLVM (clang-cl + lld)
- CMake ≥ 3.29 and Ninja
- MSVC build tools with a Windows SDK
- Vulkan SDK

```
cmake --preset release
cmake --build --preset release -- -j 6
```

This builds `build\release\ez-pc-bench.exe` (GUI) and `ez-pc-bench-cli.exe`. No developer prompt or
vcvars is needed.

## Usage

**GUI:**
1. Pick the GPU and disk(s) at the top.
2. Tick the tests you want and press **Run**.
3. **Save results...** asks for an optional run name (Enter keeps the default, e.g.
   `ezbench_20261111_113811_r7-250_rtx5060-laptop`) and writes `results\<name>.toon.txt` next to the exe.
4. **Compare...** opens the compare window. Add saved results (or drop files onto the window), the
   current run, or reference runs from the built-in **Library...** to see them side by side: an
   overall / per-suite summary, system info, raw values and the change vs a reference run.
   **Copy table** puts it on the clipboard for spreadsheets.

The buttons on the System row open **Temperatures** (live CPU zone, GPU and drive temperatures),
**Disk health** (life used, spare blocks, data written, errors from each drive's health log) and the
**settings** (gear). Each opens in its own window; hover any setting for a detailed description.

The app asks for admin rights at start. They are optional; without them a few tweaks are skipped.

**CLI:**

```
ez-pc-bench-cli --list                            # test ids
ez-pc-bench-cli --run cpu,mem --out run.toon.txt  # run suites / ids / patterns
ez-pc-bench-cli --compare run.toon.txt            # run and diff against a saved run
```

Results are plain [TOON](https://github.com/toon-format/toon) text files.

## Tips

- For stable numbers: use AC power with Windows power mode *Best performance*, and close other apps
  (games especially).
- The disk tests use one temporary file per drive (`<drive>:\ezbench.tmp`). It is deleted automatically,
  even after a crash. A full default disk run (best of 3 passes) writes about 80 GB, so use
  `--passes 1` for frequent runs.

## Reference runs

Runs I made on the PCs I've tested live in [library/](library/) and are built (compressed) into the GUI,
so you can add them from **Library...** in the compare window. The main reference is a Ryzen 7 250 /
RTX 5060 Laptop / Samsung NVMe laptop on AC, Best performance, as admin:
[library/2026-11-11_r7-250_rtx5060-laptop.toon.txt](library/2026-11-11_r7-250_rtx5060-laptop.toon.txt).

## Website

The site at <https://0xrsp.github.io/ez-pc-bench/> is plain HTML in [site/](site/), deployed by GitHub Actions
on every push to `main` that touches it. To preview it locally (needs Python 3):

```
powershell -ExecutionPolicy Bypass -File tools\serve_site.ps1
```

then open <http://localhost:8765>. The script copies the Inter fonts into `site/fonts/` (git-ignored) and
runs Python's built-in web server; stop it with Ctrl+C.

Design notes and measurements: [docs/PLAN.md](docs/PLAN.md).

MIT licensed, see [LICENSE](LICENSE). Third-party: Dear ImGui (MIT), Inter font (SIL OFL 1.1).
