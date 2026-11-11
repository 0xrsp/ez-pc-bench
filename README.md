# ez-pc-bench

A small native Windows benchmark for CPU, memory, GPU (Vulkan **and** Direct3D 12) and disks. It reports
raw numbers (GFLOPS, GB/s, ns, IOPS, FPS, ...), never synthetic "scores". GUI built with Dear ImGui, plus a
command-line version that runs the same tests. C++ (C-style), CMake, clang-cl. Single exe, no installer.

## What it measures

| Suite | Tests |
|---|---|
| **CPU** | integer ALU / multiply, FP32/FP64 FMA (scalar, AVX2, AVX-512), SGEMM, SHA-256, AES-128 (VAES), sort, prime sieve, core-to-core latency matrix, thread scaling. Each as single thread vs all threads. Kernels in inline asm, outputs self-verified. |
| **Memory** | DRAM read / write / copy (normal + non-temporal) / triad bandwidth, random-access latency 4 KiB–1 GiB, cache bandwidth sweeps (L1 → DRAM) |
| **GPU** (Vulkan + D3D12) | compute: FP32 / FP16 / INT32 throughput, VRAM and shared-memory bandwidth, PCIe upload/readback. 3D: triangles, pixel fill, texture fill, draw calls, lit scene. 2D: sprites, blit, clear. Frame tests report avg FPS, 1 % / 0.1 % lows and frame times, with a live preview in the main window. |
| **Disk** | CrystalDiskMark-style SEQ1M Q8T1 / Q1T1, RND4K Q32T1 / Q1T1 (+ NVMe profile extras), read and write, MB/s + IOPS + latency, on one or several drives |

Both GPU APIs run the same HLSL shaders (compiled to DXIL and SPIR-V at build time) and the same workloads, so
their numbers are directly comparable.

## Requirements

- Windows 10/11 x64
- [LLVM/clang](https://releases.llvm.org/) (clang-cl + lld) on `PATH`
- [CMake](https://cmake.org/) ≥ 3.29 and [Ninja](https://ninja-build.org/) (`winget install Ninja-build.Ninja`)
- Visual Studio 2022+ or Build Tools with the MSVC C++ toolset and a Windows 10/11 SDK (headers, CRT, `dxc`)
- [Vulkan SDK](https://vulkan.lunarg.com/) 1.3+ (`VULKAN_SDK` set by its installer)
- A D3D12 (feature level 12_0) and Vulkan 1.3 capable GPU driver for the GPU suites

No vcvars / developer prompt is needed: clang-cl and lld find MSVC and the Windows SDK on their own.

## Build

```
cmake --preset release
cmake --build --preset release -- -j 6
```

Output: `build\release\ez-pc-bench.exe` (GUI) and `build\release\ez-pc-bench-cli.exe`. The first command is
only needed once (or after changing `CMakeLists.txt`). Use the `debug` preset for a debug build with the
Vulkan validation layer. Shaders are rebuilt automatically when an `.hlsl` file changes.

## Run

**GUI** — `build\release\ez-pc-bench.exe`. It asks for administrator rights at start (decline = runs with a
warning; the disk test file then goes to a subfolder and a few tweaks are skipped). Pick the GPU and disk(s)
at the top, tick tests in the tree, press **Run**. **Save results** writes a `.toon.txt` file to
`results\` next to the exe; **Compare...** loads one as a baseline and adds Baseline / Δ columns.
Settings are kept in `%APPDATA%\ez-pc-bench\settings.ini`; the theme follows Windows light/dark.

**CLI** — same tests, headless:

```
ez-pc-bench-cli --list                                   # all test ids
ez-pc-bench-cli --run cpu,mem --out run.toon.txt         # run suites / ids / patterns (gpu_vk.3d.*)
ez-pc-bench-cli --run disk --disk-drives C,D --passes 2  # disk tests on two drives
ez-pc-bench-cli --compare run.toon.txt                   # run everything, show change vs a saved run
```

Other options: `--duration <ms>` (per measured phase, default 3000), `--threads <n>`, `--gpu <index>`,
`--disk-size <MiB>`, `--write-through`, `--elevate`. GUI dev flags: `--no-elevate`, `--autorun`,
`--duration <ms>`, `--only <ids>`, `--dev` (shows the smoke-test suite), `--light` / `--dark`.

## Results files

Results are saved as [TOON](https://github.com/toon-format/toon) text (`*.toon.txt`): system info and
settings as `key: value` lines, then two compact tables, `tests[N]{...}` and `metrics[M]{id,name,unit,value,x}`.
Readable in any editor and about a third smaller than the equivalent JSON. The app reads and writes them
with its own small parser (`src/core/toon.*`).

## Good to know

- **For comparable numbers** run on AC power with Windows power mode *Best performance*; the app warns
  otherwise. Laptops lose 5–15 % in later tests as they heat up.
- **Disk tests** write one test file per drive (default 1 GiB): `<drive>:\ezbench.tmp` when elevated, else
  `<drive>:\ezbench\ezbench.tmp`. It is opened delete-on-close, so it disappears even if the app crashes; a
  stale file or empty folder from a power loss is removed on the next run. A full default disk run writes
  roughly 130 GB to the SSD (5 passes) — use `--passes 1` or 2 for frequent runs.
- **GPU** tests render offscreen at 1920×1080 (configurable) on the GPU picked at the top; a downscaled
  preview is shown in the live panel. On hybrid laptops make sure the dGPU is enabled (e.g. not in an
  iGPU-only mode) or it won't be listed.

## Reference run

Lenovo laptop: Ryzen 7 250 (Zen 4, 8C/16T), 2x16 GB DDR5-5600, RTX 5060 Laptop (8 GB GDDR7), Samsung
1 TB NVMe (BitLocker on), Windows 11. AC power, *Best performance*, elevated, default settings (3 s phases,
GPU at 1920×1080, disk C: with a 1 GiB file and 5 passes). Raw data, usable as a **Compare...** baseline:
[docs/reference/2026-11-11_ac_best-performance_admin.toon.txt](docs/reference/2026-11-11_ac_best-performance_admin.toon.txt).

| CPU (1 thread → 16 threads) | |
|---|---|
| Int ALU / imul | 17.7 → 140 / 4.8 → 39 Gops/s |
| FP32 FMA AVX2 / AVX-512 | 155 → 1213 / 155 → 1239 GFLOPS |
| FP64 FMA AVX2 | 77 → 606 GFLOPS |
| SGEMM 1920² | 134 → 879 GFLOPS |
| SHA-256 / AES-128 (VAES-512) | 2.4 → 35.5 / 29.1 → 220 GB/s |
| Sort 16M / prime sieve 1e9 | 16.8 → 107 Melem/s / 2892 → 19134 Mnum/s |
| Core-to-core latency | min 12.7 (SMT siblings) / avg 23.7 / max 31.3 ns |

| Memory (1 thread → 16 threads) | |
|---|---|
| Read / write / write non-temporal | 47 → 60 / 27 → 26 / 52 → 58 GB/s (read = 67 % of 89.6 GB/s peak) |
| Copy / triad | 30 → 35 / 32 → 41 GB/s |
| Latency L1 / L2 / L3 / DRAM | 0.8 / 2.9 / 9–33 / 92–124 ns (4 KiB pages) |

| GPU (RTX 5060 Laptop) | Vulkan | D3D12 |
|---|---|---|
| FP32 / FP16 / INT32 | 14.7 / 16.0 TFLOPS / 9.2 TIOPS | 14.5 / 17.5 / 9.2 |
| VRAM read / write / copy | 368 / 347 / 338 GB/s | 369 / 368 / 338 |
| Shared memory / PCIe up / down | 8.6 TB/s / 14.2 / 14.1 GB/s | 8.5 / 14.1 / 13.9 |
| Triangles / pixel fill / texture fill | 2.65 Gtri/s / 107 Gpix/s / 180 Gtex/s | 2.56 / 104 / 174 |
| Draw calls | 31.6 M/s | 65.5 M/s |
| Lit scene (avg / 1 % low) | 7490 / 3674 fps | 6826 / 3471 |
| Sprites | 80.0 Msprites/s | 77.9 |

| Disk C: (read / write) | MB/s | IOPS |
|---|---|---|
| SEQ1M Q8T1 | 7025 / 5889 | |
| SEQ1M Q1T1 | 2811 / 2344 | |
| RND4K Q32T1 | 427 / 734 | 104K / 179K |
| RND4K Q1T1 | 60.5 / 121 | 14.8K / 29.6K (68 / 34 µs) |

The prime sieve result is from a re-run after a build fix (see PLAN.md §10); everything else is from one
full 53-test run. More numbers and sanity checks against hardware peaks are in [docs/PLAN.md](docs/PLAN.md) §10.

## Project layout

```
src/app/     GUI (main_gui, ui, theme, settings) and CLI (main_cli)
src/core/    registry, runner, results / TOON, sysinfo, threads, perf tweaks, baseline compare
src/cpu/ src/mem/ src/disk/   suites
src/gpu/     shared contract (gpu_common.h), D3D12 and Vulkan backends, shaders/*.hlsl
docs/        PLAN.md (design notes, decisions, measurements), reference/ (reference runs)
third_party/ Dear ImGui (MIT), Inter font (SIL OFL 1.1)
```

Design notes, the decision log and all measured findings are in [docs/PLAN.md](docs/PLAN.md).
