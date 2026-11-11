# ez-pc-bench — Plan & Knowledge Base

Living doc. Update it as decisions are made or facts are learned. Keep it terse.

## 1. Goal

A small, fast, native Windows benchmark for this laptop. GUI: the user picks suites/tests in a
collapsible tree, hits Run, sees live progress + live stats of every test as the run moves through
them, and gets a results table of **raw numbers** (GB/s, GFLOPS, IOPS, ns, ms, FPS, ...). No synthetic
"scores". A CLI build runs the same benchmarks headless and writes JSON.

Suites: **CPU**, **Memory**, **GPU** (Vulkan + Direct3D 12; 2D, 3D, Compute), **Disk** (CrystalDiskMark-style).

Scope: only needs to work on this machine for now. No installer, no cross-platform.

## 2. Target machine (measured 2026-11-11)

| Item | Value |
|---|---|
| CPU | AMD Ryzen 7 250 (Zen 4 "Hawk Point"), 8C/16T, single CCX. AVX2 + AVX-512 (256-bit datapath, double-pumped) |
| Caches (expected for Zen 4 mobile) | L1D 32 KiB/core, L2 1 MiB/core, L3 16 MiB shared — read via CPUID at runtime, never hardcode |
| RAM | 2 × 16 GiB Samsung M425R2GA3EB0-CWMOL, **DDR5-5600 SO-DIMM**, dual channel → theoretical peak 89.6 GB/s |
| dGPU | NVIDIA GeForce RTX 5060 Laptop (Blackwell), driver 32.0.16.2114. **No display attached** (Optimus) |
| iGPU | AMD Radeon 780M (RDNA3). **Drives the internal panel: 1920×1080 @ 144 Hz** |
| Disk | Samsung MZAL81T0HFLB 1 TB NVMe (C:, NTFS) |
| K: drive | 250 GB **ReFS Dev Drive backed by a VHD file on C:** — repo lives here. Not a default disk-bench target (measures VHD + ReFS overhead) |
| Power at review time | On battery, "Balanced" scheme — reference runs must be on AC + best performance |
| OS | Windows 11 Pro 10.0.26300. `wmic` is not present (removed) |

Consequences: dGPU frames presented to the panel go through a cross-adapter copy to the iGPU (same as
games on this laptop). The UI's D3D11 device will likely land on the iGPU, leaving the dGPU clean.

## 3. Toolchain (measured 2026-11-11)

| Tool | Location / version |
|---|---|
| clang / clang-cl | 23.1.5, standalone LLVM at `C:\software\clang+llvm-23.1.5-x86_64-pc-windows-msvc\bin` (on PATH). Target `x86_64-pc-windows-msvc` |
| CMake | 4.5.0-rc3 (`C:\Program Files\CMake\bin`) — generators incl. Ninja, VS 18 2026 |
| MSVC | VS 2026 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`), tools 14.52.37025 |
| VS clang-cl toolset (`VC\Tools\Llvm`) | **Not installed** → `-T ClangCL` with VS generator won't work out of the box |
| Windows SDK | 10.0.26100.0. `bin\10.0.26100.0\x64` has `dxc.exe` (1.9.2602) **+ `dxil.dll`** (signs DXIL), `fxc.exe` |
| Vulkan SDK | 1.4.363.1 at `C:\VulkanSDK\1.4.363.1`; `VULKAN_SDK` set machine-wide (needs a new shell). `dxc.exe` (1.9.0.5512, SPIR-V codegen) but **no `dxil.dll`**; `glslc.exe` |
| Ninja | 1.13.2 via winget (user PATH; open a new shell after install) |
| git | 2.56.1 |

### Build approach (working since M0)

- Generator **Ninja**, compiler **clang-cl** (MSVC ABI, MSVC CRT + Windows SDK), linker `lld-link`
  (`CMAKE_LINKER_TYPE=LLD`), resources `llvm-rc`.
- **No vcvars needed**: clang-cl and lld-link find MSVC + the Windows SDK on their own. The manifest is
  embedded through `app.rc` (`/MANIFEST:NO`), so `mt.exe` isn't needed either.
- `CMAKE_CXX_SCAN_FOR_MODULES OFF` (CMake 4 + C++20 would otherwise scan the MSVC `std` module every build);
  CMake's default `/EHsc` is stripped.
- `CMakePresets.json`: `debug` and `release` (`build/debug`, `build/release`):
  `cmake --preset release` then `cmake --build --preset release`.
- Targets: `ezbench_core` (static lib: all benchmarks, sysinfo, runner), `ez-pc-bench` (GUI,
  `/SUBSYSTEM:WINDOWS`), `ez-pc-bench-cli` (console). Two exes avoid the AttachConsole mess.
- Release: `/O2 /arch:AVX2` for app code. AVX-512 kernels per-function via
  `__attribute__((target("avx512f,avx512vl,...")))`, selected by runtime CPUID.
- `/EHs-c- -D_HAS_EXCEPTIONS=0 /GR-`, static CRT (`/MT`), `/W4`, warnings clean.
- Manifest: `asInvoker` + per-monitor-v2 DPI awareness + `longPathAware`. Elevation handled at runtime (§5.4).
- Exe sizes at M0 (release): GUI 846 KB, CLI 223 KB.

### Shader build

- One HLSL source per shader in `src/gpu/shaders/`.
- **DXIL**: Windows SDK `dxc.exe` (ships `dxil.dll` → signed/validated DXIL that D3D12 accepts).
- **SPIR-V**: Vulkan SDK `dxc.exe -spirv -fspv-target-env=vulkan1.3`.
- CMake custom commands compile both at build time and embed the bytecode as byte arrays in
  generated headers. Re-verify DXIL signing works when M4 starts.

## 4. UI library decision

| Option | Verdict |
|---|---|
| **Dear ImGui** | **Chosen.** Immediate mode fits "poll shared state every frame". `CollapsingHeader`/`TreeNode` + `Checkbox` for the tree, `ProgressBar`, `BeginTable`, `PlotLines`/`PlotHistogram`. Mature Win32 + D3D11 backends, C-ish API. |
| ImPlot (addon) | Optional, only if `PlotLines` isn't enough for live graphs / latency curves / frame-time plots. |
| Nuklear / microui | Smaller ecosystems, weaker tables/plots, more manual work. |
| RmlUi / Qt / wxWidgets / WinUI | Too heavy or OOP/retained; against the lightweight C-style goal. |
| Raw Win32 controls | Lightest, but painful for live graphs/tables. |

- UI renderer: **ImGui Win32 + D3D11**, independent from the D3D12/Vulkan benchmark devices.
- Vendor a pinned Dear ImGui release (`master`, not docking) in `third_party/imgui/` (core +
  `imgui_impl_win32` + `imgui_impl_dx11`). Record version in `third_party/README.md`.
- High DPI: per-monitor-v2 awareness, scale fonts/style by `GetDpiForWindow`, rebuild font atlas on
  `WM_DPICHANGED`.
- Interference rules: while a test runs, UI renders at ~10–15 FPS (waitable timer), VSync on; when
  idle, render on input / low rate. UI on the main thread, benchmarks on one worker thread (plus its
  own pinned pool). The worker's pool avoids the core the UI thread is on only for ST tests.

## 5. Architecture

Procedural, data-oriented, C-style C++ (rules in CLAUDE.md).

```
src/
  app/main_gui.cpp     Win32 window, D3D11 device/swapchain, ImGui frame loop
  app/main_cli.cpp     Arg parsing, runs selected tests, prints live progress lines, writes JSON
  app/ui.cpp/.h        Suite tree, settings, live panel, results/warnings
  app/app.rc/.manifest Embedded manifest (asInvoker, PMv2 DPI, long paths)
  core/types.h         u8..u64, f32/f64, KiB/MiB/GiB
  core/bench.h         BenchParams, BenchLive, BenchResult, Metric, SuiteDesc, BenchDesc
  core/registry.cpp    The suite + test tables (only place new tests are registered)
  core/runner.cpp/.h   Worker thread: suite init → tests → suite shutdown, cancel
  core/results.cpp/.h  Metric arena, result helpers (res_*), text + JSON export
  core/sysinfo.cpp/.h  CPUID, SMBIOS RAM, adapters, power, OS, admin (drives/BitLocker: M3)
  core/perf.cpp/.h     Process/thread perf setup (priority, throttling opt-out, timer res, keep awake)
  core/elevation.cpp/.h  is_elevated, relaunch via runas
  core/smoke_bench.cpp Smoke suite: exercises runner, live stats and all three result kinds
  core/timer.cpp/.h    QPC wrapper, rdtsc, do_not_optimize/clobber_memory
  core/threads.cpp/.h  Minimal pinned thread pool (Win32 threads, group affinity, barriers)
  core/mem_util.h      VirtualAlloc helpers, large pages (best effort), arena
  cpu/cpu_bench.cpp  cpu/cpu_kernels.cpp (inline-asm kernels)
  mem/mem_bench.cpp
  disk/disk_bench.cpp
  gpu/gpu_common.h     Shared test params, frame-time stats, adapter selection
  gpu/gpu_window.cpp   Benchmark render window (separate from UI)
  gpu/gpu_vk.cpp       Vulkan backend
  gpu/gpu_d3d12.cpp    D3D12 backend
  gpu/shaders/*.hlsl
third_party/imgui/
docs/PLAN.md
```

### 5.1 Core data model (sketch)

```cpp
enum SuiteId : u8 { SUITE_CPU, SUITE_MEM, SUITE_GPU_VK, SUITE_GPU_D3D12, SUITE_DISK, SUITE_COUNT };
enum ResultKind : u8 { RESULT_VALUES, RESULT_SERIES, RESULT_MATRIX };
enum BenchStatus : u8 { ST_PENDING, ST_RUNNING, ST_DONE, ST_SKIPPED, ST_FAILED, ST_CANCELLED };

struct Metric { const char* name; const char* unit; f64 value; };    // static strings only

struct BenchResult {                     // values live in a global Metric arena
    u32 first, count;                    // slice into g_metrics[]
    ResultKind kind;                     // SERIES: metrics are (x label, y) points; MATRIX: rows*cols
    u16 rows, cols;
    BenchStatus status;
    f64 seconds;                         // wall time the test took
};

struct BenchParams {                     // settings snapshot taken at Run
    u32 duration_ms;                     // per measured phase
    u32 thread_count;                    // 0 = all logical
    u32 gpu_adapter;                     // index into sysinfo adapter list
    u32 gpu_width, gpu_height; bool gpu_fullscreen;
    wchar_t disk_dir[260]; u64 disk_file_bytes; u32 disk_passes; bool disk_write_through;
};

struct BenchLive {                       // single writer (worker), single reader (UI)
    std::atomic<f32> progress;           // 0..1 within current test
    std::atomic<f64> current;            // latest instantaneous value
    std::atomic<u32> phase;              // index into the test's phase-name table
    std::atomic<f32> history[256];       // relaxed ring buffer for the live sparkline
    std::atomic<u32> history_head;
};

typedef bool (*SuiteInitFn)(const BenchParams*, void** suite_state);   // e.g. GPU device, disk test file
typedef void (*SuiteShutdownFn)(void* suite_state);
typedef void (*BenchFn)(void* suite_state, const BenchParams*, BenchLive*, BenchResult*,
                        const std::atomic<bool>* cancel);

struct SuiteDesc { SuiteId id; const char* name; SuiteInitFn init; SuiteShutdownFn shutdown; };
struct BenchDesc { SuiteId suite; const char* group; const char* name; const char* cli_id;
                   BenchFn fn; u32 flags; };

extern const SuiteDesc g_suites[SUITE_COUNT];
extern const BenchDesc g_benches[];      // one static table = the whole registry
// Parallel arrays by bench id: g_selected[], g_results[], g_status[]. One BenchLive (one test runs at a time).
```

- No virtuals: a suite is a pair of function pointers, a test is a function pointer + a table row.
- GPU suites share test functions; the API-specific backend lives in `suite_state`.
- Tree shape: Suite → group → test. GPU appears as two suites: **GPU (Vulkan)** and **GPU (D3D12)**,
  each with groups Compute / 3D / 2D.

### 5.2 Runner flow

1. Build the run list from `g_selected` (GUI) or `--run` patterns (CLI), in registry order.
2. Worker: apply perf setup (§5.4) → for each suite with selected tests: `init` once → each test:
   reset `BenchLive`, status RUNNING, call `fn`, store result/status → `shutdown` → restore perf state.
3. Suite `init` failure (e.g. no Vulkan device) marks that suite's tests FAILED with a reason, run continues.
4. Cancel: atomic flag checked between iterations; suite shutdown still runs (deletes disk file, frees GPU).

### 5.3 GUI vs CLI

- **GUI**: tree with per-test status glyph, per-test progress bar while running, final primary value
  inline. Live panel: current test, phase, current value + unit, sparkline (GPU 2D/3D: live FPS and
  frame-time plot), overall progress (tests done / total + current test fraction). **No ETA.**
  End: results table (Suite | Test | Metric | Value | Unit), series/matrix results as small tables or
  plots, "Copy as text", "Save JSON/CSV".
- **CLI** (`ez-pc-bench-cli.exe`):
  `--list` (all test ids), `--run cpu,mem.latency,gpu_vk.compute.*` (patterns), `--json out.json`,
  `--csv out.csv`, `--gpu <index|name>`, `--disk-dir`, `--disk-size`, `--passes`, `--duration`,
  `--write-through`. Prints one progress line per test (updating in place), exit code ≠ 0 on failure.
  Main tool for automated verification during development.

### 5.4 Elevation & performance setup

Elevation:
- Manifest stays `asInvoker`. On GUI start, if not elevated → **prompt**: relaunch self via
  `ShellExecuteExW` verb `runas` (UAC dialog). If the user declines, continue non-elevated with a
  **persistent warning banner** listing what's degraded.
- CLI never relaunches (would lose the console): prints a warning when not elevated; `--elevate`
  flag optionally relaunches with `runas` in a new window.
- Admin gives: test file in drive root, priority/power tweaks that need it, WMI BitLocker method query.

Perf setup during a run (restored afterwards):
- `SetPriorityClass(HIGH_PRIORITY_CLASS)` (never REALTIME); worker threads `THREAD_PRIORITY_HIGHEST`.
- Opt out of power throttling / EcoQoS: `SetProcessInformation(ProcessPowerThrottling)` with
  `EXECUTION_SPEED` control + state 0, and `SetThreadInformation(ThreadPowerThrottling)` per thread.
- Thread pinning with `SetThreadGroupAffinity` (hard pin for measurements); ST tests on the
  highest-ranked core from `GetSystemCpuSetInformation` (EfficiencyClass / SchedulingClass), UI thread
  pinned away from it for the duration. MT tests: physical cores first, then SMT siblings (topology from
  `GetLogicalProcessorInformationEx`).
- `timeBeginPeriod(1)` for the run (precise sleeps/intervals), `timeEndPeriod` after.
- `SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED | ES_CONTINUOUS)` so the laptop
  doesn't sleep/dim mid-run.
- `SetProcessWorkingSetSizeEx` raised so big buffers stay resident; pre-fault all buffers.
- Large pages (`MEM_LARGE_PAGES`) best effort — needs `SeLockMemoryPrivilege`, which admins don't have
  by default; enable if present, else silently use 4 KiB pages and note it in results.
- Power: warn (don't change) if on battery or the active power scheme/overlay isn't best performance
  (`GetSystemPowerStatus`, `PowerGetActiveScheme`, `PowerGetEffectiveOverlayScheme`).

## 6. Benchmark catalog

Units are raw numbers. MT tests use all logical cores unless the thread override is set; ST variants
where meaningful.

### 6.1 CPU

All hot kernels are written in **inline asm** (`__asm__ volatile`, clang-cl supports GNU syntax) or
intrinsics + a sink, so the compiler can't vectorize "scalar" tests or fold chains. Unroll counts and
accumulator counts are explicit.

| Test | What | Metric |
|---|---|---|
| Integer ALU | Independent add/xor/imul streams, ST + MT | Gops/s |
| FP32 FMA scalar | `vfmadd231ss` × ≥8 independent accumulators | GFLOPS |
| FP32 FMA AVX2 | `vfmadd231ps ymm` × ≥10 accumulators | GFLOPS |
| FP32/FP64 FMA AVX-512 | `zmm` FMA (runtime-gated by CPUID) | GFLOPS |
| SGEMM | Cache-blocked AVX2/AVX-512 matmul, 2048², MT | GFLOPS |
| Hashing | SHA-256 with SHA-NI (scalar fallback) | GB/s |
| AES | AES-NI / VAES CTR encrypt | GB/s |
| Sort | Sort 16M u32 (ST + parallel merge sort MT) | Melem/s |
| Prime sieve | Segmented sieve, branch-heavy | Mnum/s |
| Core-to-core latency | Cache-line ping-pong for each LP pair → **matrix** | ns |
| Thread scaling | AVX2 FMA at 1, 2, 4, 8, 16 threads → **series** | GFLOPS |

### 6.2 Memory

| Test | What | Metric |
|---|---|---|
| Read BW | AVX2 loads over 1 GiB, MT | GB/s |
| Write BW | Normal stores and non-temporal (`vmovntdq`) stores | GB/s |
| Copy BW | Normal + NT copy | GB/s |
| Triad (STREAM) | a = b + s·c | GB/s |
| Latency | Random pointer chase (shuffled cycle, one hop per cache line), 16 KiB … 1 GiB → **series** | ns |
| Cache BW sweep | Read BW per working-set size, ST + MT → **series** | GB/s |

Theoretical DRAM peak for reference: 89.6 GB/s (DDR5-5600 dual channel). Expect well below it on
single-CCX Zen 4 (fabric-limited reads).

### 6.3 GPU (Vulkan **and** D3D12, same tests, same HLSL)

**Adapter selection**: one adapter for both GPU suites, chosen explicitly in settings (GUI combo /
CLI `--gpu`). Default = most powerful: first adapter from
`IDXGIFactory6::EnumAdapterByGpuPreference(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)`, tie-break by
`DedicatedVideoMemory`. Vulkan device matched by **LUID** (`VkPhysicalDeviceIDProperties.deviceLUID`).
Never export `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` (would move the UI to the dGPU).

**Timing source is per test** (stated in each test's metadata):
- *Compute*: GPU timestamp queries (`GetTimestampFrequency` / Vulkan `timestampPeriod`) around each
  dispatch batch → **actual timings**: min / avg / max ms per dispatch, plus derived throughput.
- *2D / 3D*: CPU wall clock (QPC) between consecutive presents, like PresentMon's MsBetweenPresents.
- *Transfers*: wall clock for the whole submit→fence round trip, GPU timestamps as a cross-check.

**2D and 3D tests render on screen** in a separate benchmark window (not the UI window):
- Borderless fullscreen at the panel's native resolution by default (1920×1080), or windowed at a
  configurable size. Created by the GPU suite on the worker thread, with its own message pump.
- Uncapped: DXGI flip-discard + `DXGI_PRESENT_ALLOW_TEARING`; Vulkan `IMMEDIATE` (fallback `MAILBOX`).
- On this laptop dGPU frames are copied to the iGPU for display — inherent, same as games; note in results.
- Each test: ~2 s warm-up (discarded), then fixed duration (default 15 s) of frame times.
- Reported: **avg FPS**, **1% low**, **0.1% low**, avg / p99 / p99.9 frame time (ms), plus the derived
  throughput (Mtri/s, Gpix/s, ...). Definition used: *1% low = 1000 / (99th-percentile frame time in ms)*,
  *0.1% low = 1000 / (99.9th-percentile)*. Frame-time history is plotted live.

Compute (timings + throughput):
| Test | Derived metric |
|---|---|
| FP32 FMA (`mad` chains, many waves) | TFLOPS |
| FP16 packed (SM 6.2 `-enable-16bit-types`, Vulkan `shaderFloat16`; skip if unsupported) | TFLOPS |
| INT32 | TIOPS |
| VRAM bandwidth (buffer read / write / copy, 1 GiB) | GB/s |
| Groupshared (LDS) bandwidth | GB/s |
| Host↔device transfer (upload & readback via copy queue) | GB/s |

3D (FPS + derived):
| Test | Derived metric |
|---|---|
| Triangle throughput (dense instanced mesh, tiny tris, trivial PS) | Mtri/s |
| Pixel fill (full-screen quads with overdraw, RGBA8, opaque + blended; per-pixel varying color so color compression can't inflate it) | Gpix/s |
| Texture fill (bilinear, multiple textures, non-constant texel data) | Gtex/s |
| Draw-call throughput (thousands of small draws/frame, CPU submit bound) | draws/s |
| Scene (instanced lit geometry + depth, several passes) | — (FPS only) |

2D (FPS + derived):
| Test | Derived metric |
|---|---|
| Sprite batch (alpha-blended textured quads, many layers) | Msprites/s |
| Blit / texture copy (RT→RT, `CopyResource` / `vkCmdBlitImage`) | GB/s |
| Clear rate (render-target clears) | GB/s |
| (Stretch) Direct2D path: geometry fill + text, D3D12 suite only via D3D11On12 | ops/s |

Debug builds enable the Vulkan validation layer and the D3D12 debug layer.

### 6.4 Disk (CrystalDiskMark-style)

Default profile mirrors CDM 8 "Default":
| Test | Block | Queue depth | Threads |
|---|---|---|---|
| SEQ1M Q8T1 read / write | 1 MiB | 8 | 1 |
| SEQ1M Q1T1 read / write | 1 MiB | 1 | 1 |
| RND4K Q32T1 read / write | 4 KiB | 32 | 1 |
| RND4K Q1T1 read / write | 4 KiB | 1 | 1 |
Optional NVMe profile extras: SEQ128K Q32T1, RND4K Q32T16. Optional mixed 70/30.

Metrics: MB/s (decimal, like CDM), IOPS, average latency (µs). 5 passes × ~5 s, interval between
passes, report best (CDM default).

Implementation:
- Target drive selectable (default `C:`), warn if the target volume is a VHD/Dev Drive (like K:).
- File location: elevated → `<drive>:\ezbench.tmp` (like CDM); not elevated → `<drive>:\ezbench\ezbench.tmp`
  (users can create folders in a drive root but not files), falling back to `%TEMP%` if that fails.
  Default size 1 GiB (64 MiB … 8 GiB). Created once in suite `init`, deleted in `shutdown`; a stale file
  from a crashed run is deleted at next start.
- `CreateFileW` with `FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED` (bypass OS cache, like DiskSpd `-S`).
  **`FILE_FLAG_WRITE_THROUGH` only when opted in** (it issues FUA writes and tanks consumer-NVMe write
  numbers; CDM doesn't use it by default).
- Buffers from `VirtualAlloc` (page-aligned). Offsets/sizes multiples of the physical sector size
  (`IOCTL_STORAGE_QUERY_PROPERTY` → `StorageAccessAlignmentProperty`).
- QD = N outstanding OVERLAPPED requests per thread, completion via IOCP
  (`GetQueuedCompletionStatusEx`), resubmit on completion.
- Prepare: write the whole file sequentially with **random (incompressible) data** in `init`. Writes
  that extend a file's valid data length are synchronous on NTFS even with OVERLAPPED — another reason to
  fully write the file before any timed test.
- Random offsets: xorshift/pcg, block-aligned across the whole file. Check free space first.

## 7. System info & warnings

Shown in the GUI header and in every JSON/CSV export:
- CPU: name, cores/threads, caches, ISA features (CPUID).
- **RAM**, natively without WMI: totals via `GlobalMemoryStatusEx` / `GetPhysicallyInstalledSystemMemory`;
  per-module type, speed, configured speed, manufacturer, part number, size by parsing the raw SMBIOS
  table from `GetSystemFirmwareTable('RSMB', 0, ...)` → **Type 17 (Memory Device)** records. No admin needed.
- GPUs: DXGI adapter list (name, VRAM, LUID, driver version via `CheckInterfaceSupport`/registry).
- Drives: model, bus, volume FS, Dev Drive/VHD detection (`IOCTL_STORAGE_QUERY_PROPERTY` bus type
  `BusTypeFileBackedVirtual`).
- **BitLocker status** per volume: shell property `System.Volume.BitLockerProtection` (no admin); when
  elevated also the encryption method via WMI `Win32_EncryptableVolume` (COM, native) to show
  software vs hardware encryption.
- OS build, power source, active power scheme + overlay, elevation state.

Warning banners: not elevated; on battery; power scheme/mode not best performance; disk target is a
VHD/Dev Drive; BitLocker on target volume; large pages unavailable (info only).

## 8. Milestones

- **M0 Skeleton** — ✅ done 2026-11-11: CMake + presets, vendored ImGui 1.93.0, GUI window (DPI aware),
  CLI exe, registry with suite init/shutdown, runner thread, metric arena, smoke suite (values / series /
  matrix) with live progress, cancel, results table, JSON export + copy text, elevation prompt + banners,
  sysinfo (CPUID, cache topology, SMBIOS RAM, DXGI GPUs, power plan + mode, OS).
  Dev flags: GUI `--no-elevate` (skip UAC prompt), `--autorun` (start default selection at launch),
  `--duration <ms>`.
- **M1 CPU** — ✅ done 2026-11-11: 14 tests (`cpu.*`), inline-asm int/FMA kernels, AVX2 SGEMM, SHA-NI,
  AES-NI/VAES-512, sort, sieve, core-to-core matrix, thread scaling series. Every test with checkable
  output self-verifies (known-answer tests, GEMM spot check, sortedness, π(1e9)). Pinned pool in
  `core/threads.*`, generic ST/MT throughput driver in `cpu_bench.cpp`. GUI got a Charts tab
  (series plots + heat-mapped matrix). Not done: pinning the UI thread away from the ST core
  (UI at 15 FPS is negligible; revisit if ST numbers look noisy).
- **M2 Memory** — ✅ done 2026-11-11: 7 tests (`mem.*`): read / write / copy (normal + non-temporal) /
  triad bandwidth ST+MT over ≥ 512 MiB, random pointer-chase latency 4 KiB–1 GiB (series, large pages
  when the privilege exists), read-bandwidth sweeps 16 KiB–256 MiB for one thread and per-thread for all
  threads. AVX2 inline-asm kernels in `mem/mem_kernels.cpp`; throughput driver moved to
  `core/throughput.*` (shared with CPU). Memory safety: the suite arena is min(1.5 GiB, ¼ of free RAM)
  and fails cleanly below 256 MiB; peak process RAM in a full run ≈ 1.6 GB.
- **M3 Disk** — ✅ done 2026-11-11 (written by a subagent in a worktree, reviewed + merged): 6 tests
  (`disk.*`): CDM 8 Default profile + NVMe extras (SEQ128K Q32T1, RND4K Q32T16, off by default), each
  read then write, best of N passes, MB/s + IOPS + avg latency. One NO_BUFFERING|OVERLAPPED handle and
  IOCP per thread, QD outstanding requests, `CancelIoEx` drain on stop. Test file is created with
  `FILE_FLAG_DELETE_ON_CLOSE` and held open until suite shutdown, so even a killed process leaves no
  file; stale `<drive>:\ezbench\` is removed at the next start; `%TEMP%` fallback only if on the target
  drive. Drive info (model, bus, FS, sectors, VHD/Dev Drive, BitLocker) in sysinfo, GUI, CLI and exports.
  **Deviation:** groups are "Sequential"/"Random" and every sequential test runs before any random one
  (random 4K writes slowed later sequential reads, see §13).
- **M4 GPU infra + Compute**: shader build (DXIL + SPIR-V), adapter selection, D3D12 + Vulkan devices,
  timestamps, compute tests.
- **M5 GPU 3D + 2D**: benchmark window, swapchains (tearing), frame-time stats, 3D and 2D tests.
- **M6 Polish**: CSV, copy to clipboard, settings persistence (ini), compare against a saved JSON.

Each milestone: builds warning-free, runs via CLI on this machine, numbers sanity-checked (§10).

## 9. Benchmarking pitfalls (knowledge)

- **Power/thermals**: laptop boost decays over seconds; on battery or "Balanced" numbers drop. Warm-up,
  fixed duration, report best + median; keep test order stable.
- **Timers**: QPC (10 MHz) for wall time. `rdtsc` invariant on Zen 4 but runs at a fixed reference
  frequency, not core clock — short relative timing only.
- **Compiler**: scalar loops get auto-vectorized under `/arch:AVX2` and chains can be folded → kernels in
  inline asm; every result feeds a sink. Inspect generated asm for benchmark TUs.
- **Topology**: Zen 4 SMT siblings are usually adjacent LPs (0/1, 2/3, …) — verify via
  `GetLogicalProcessorInformationEx`. Use group-aware APIs even with one group.
- **Memory**: Windows zeroes pages on first touch — pre-fault before timing.
- **Disk**: NVMe SLC cache inflates sequential writes; 1 GiB default is CDM-like. Defender scans on
  file create — one file reused across tests. BitLocker software encryption costs throughput + CPU.
- **GPU**: first dispatch/draw includes pipeline/driver warm-up — always warm up. Can't lock clocks on
  consumer drivers → longer runs. Device must be created on the chosen adapter (DXGI adapter / Vulkan
  LUID match). Constant-color fills are inflated by delta color compression. Optimus: dGPU presents via
  copy to the iGPU.
- **Frame pacing stats**: 0.1% low needs enough frames — at least ~5000 frames per test; extend the
  duration automatically if FPS is low.

## 10. Sanity reference values (fill with measured numbers)

| Metric | Expected ballpark |
|---|---|
| DRAM read BW (MT) | below 89.6 GB/s theoretical; single-CCX fabric limit likely ~60–70 GB/s |
| DRAM latency | ~90–130 ns typical for mobile Zen 4 |
| NVMe SEQ1M Q8T1 read | several GB/s (PCIe 4.0 x4 class) — measured ~7.0 GB/s on battery |
| RTX 5060 Laptop FP32 | several TFLOPS (depends on TGP/clock) |

## 11. Open questions

None currently. (All earlier ones resolved — see decision log.)

## 12. Decision log

- 2026-11-11: UI = Dear ImGui (Win32 + D3D11), vendored. Build = CMake + Ninja + clang-cl.
  GPU benches = Vulkan + D3D12 from shared HLSL. C-style, data-oriented; no exceptions/RTTI.
- 2026-11-11 (plan review):
  - Elevation: prompt via `runas` relaunch on start; warn persistently if declined. CLI warns only.
  - Disk write-through is opt-in; default matches CDM (no OS cache, no FUA).
  - CPU kernels in inline asm.
  - DXIL from Windows SDK dxc (has `dxil.dll`), SPIR-V from Vulkan SDK dxc.
  - GPU 2D/3D render on screen in a benchmark window; report avg FPS, 1% low, 0.1% low (+ frame times,
    derived throughput). Compute reports GPU-timestamp timings + derived throughput.
  - RAM details from raw SMBIOS (`GetSystemFirmwareTable`), not WMI.
  - Variable-length results (metric arena + series/matrix kinds); suite init/shutdown hooks.
  - CLI build (`ez-pc-bench-cli.exe`) for headless runs and verification.
  - GPU suites as two top-level suites (Vulkan, D3D12) sharing test functions.
  - Perf setup: high priority, pinning, power-throttling opt-out, timer resolution, no-sleep, working set.
  - No ETA: live overall + per-test progress and status only.
  - Power warnings for battery and non-performance power scheme/mode (warn only, never change settings).
  - Explicit GPU selection for GPU suites, default = most powerful adapter.
  - High-DPI aware UI. BitLocker status shown.
  - D3D12 chosen as the DirectX API; one adapter per run; results compare in M6.
- 2026-11-11 (UI theme, trial): Material 3 look in `app/theme.*` — light (default) + dark palettes from
  seed #0B57D0, flat page + white outlined cards, pill buttons, Material checkboxes (`ImGuiCol_CheckboxSelectedBg`
  is the checked fill in 1.93), segmented Results/Charts tabs, data table with horizontal dividers only.
  Font: Inter 4.1 Regular/Medium embedded as RCDATA (exe 846 KB → 1.7 MB), rasterized sharp: OversampleH/V 1,
  PixelSnapH, font sizes snapped to whole pixels after DPI scaling. Dark mode toggle in Settings
  (`--dark` flag for screenshots). Replaced the earlier violet/starfield theme at the user's request.
- 2026-11-11 (M0):
  - Disk target is a drive letter (`BenchParams::disk_drive`); the file path is derived per §6.4.
  - `BenchLive` carries `phase`/`unit` as static-string atomics instead of a phase index table.
  - Smoke suite kept as a permanent runner self-test (`smoke.*`).
  - GUI verification: launch with `--no-elevate --autorun` and capture the window with `PrintWindow`.

## 13. Measured notes

- Smoke `smoke.matrix` (memcpy 4 MiB, src/dst 64 MiB apart): ~3 GB/s instead of ~30 GB/s whenever the
  dst offset is ahead of src by 16–48 B. A real Zen 4 `rep movsb` aliasing effect — remember it when
  designing the memory copy test (use own kernels, control relative alignment).
- CRT `memset` switches to non-temporal stores somewhere above 32 MiB (write BW jumps 14 → 39 GB/s on battery).
- CPU suite on battery / "Best power efficiency" (~2.4 GHz effective), 1 s phases, ST → MT:
  int ALU 8.6 → 72 Gops/s (≈3.6 ops/clk, 4 ALUs), imul 2.3 → 19.7 Gops/s (1/clk),
  FP32 FMA scalar 9.7 → 78.6, AVX2 FP32 75 → 629, AVX2 FP64 35 → 313, AVX-512 FP32 71 → 628 GFLOPS
  (Zen 4 double-pumps zmm: same FLOPs/clk as AVX2), SGEMM 67 → 566 GFLOPS (~89 % of FMA peak),
  SHA-256 1.21 → 19.0 GB/s (MT/ST 15.7x: latency-bound, SMT helps), AES-128 VAES-512 14.6 → 123 GB/s,
  sort 8.3 → 54.8 Melem/s, sieve 1448 → 10699 Mnum/s. FMA scaling linear to 8 threads, SMT adds ~0.
  Re-measure on AC + Best performance.
- Core-to-core: spinning **without** `pause` inflated everything (cross-core 60–70 ns, SMT 42–52 ns).
  With `_mm_pause()` in both spin loops: cross-core ~43–50 ns, SMT siblings ~27–40 ns. Kept `pause`.
- Throughput live rate uses a ~0.5 s sliding window; without it single-thread SGEMM (0.2 s per call)
  showed 4x spikes.
- Memory suite on battery / "Best power efficiency": read 37 (ST) / 46 (MT) GB/s = 52 % of the
  89.6 GB/s peak; write 19 / 20.5 (normal, RFO) vs 36 / 45 GB/s non-temporal; copy 20 / 28 vs 37 / 43;
  triad 22 / 31 GB/s. Latency (4 KiB pages): L1 1.7 ns (~4 clk), L2 6.2 ns, L3 27–54 ns (2–8 MiB, TLB
  misses grow), DRAM 105–182 ns (32 MiB–1 GiB). Sweep: L1 134 GB/s ST (~56 B/clk) / 1.17 TB/s MT,
  L2 ~70 / ~630 GB/s, DRAM ~35 / ~47 GB/s.
- Sweep/latency points shorter than ~0.25 s gave isolated dips/spikes on battery (clock ramping);
  minimum per point is now 0.25 s.
- Drives: C: = SAMSUNG MZAL81T0HFLB-00BL2, NVMe, NTFS, sectors 512 logical / 4096 physical, **BitLocker
  on**. K: = "Msft Virtual Disk", bus File-backed virtual, ReFS Dev Drive, BitLocker off (but its VHD file
  lives on encrypted C:). Dev Drive detection needs `FSCTL_QUERY_PERSISTENT_VOLUME_STATE` on a root
  *directory* handle; `fsutil devdrv query` needs admin.
- Disk on C: (battery, efficiency mode, 256 MiB file, 2 passes): SEQ1M Q8T1 7.0 / 5.4 GB/s read/write,
  SEQ1M Q1T1 2.3 / 2.8, SEQ128K Q32T1 5.9 / 4.8, RND4K Q32T1 211 / 330 MB/s (51K / 81K IOPS),
  RND4K Q1T1 32–46 / 61–67 MB/s, RND4K Q32T16 2.0 GB/s read (483K IOPS). RND4K Q32T1 is CPU-bound on the
  single submitting thread in this power state (~20 µs/request; T16 shows the SSD isn't the limit).
- Random 4K writes over the small test file cut a later SEQ128K Q32T1 read from ~6.4 to ~1.5 GB/s on this
  SSD (reproduced twice) → sequential tests always run first.
- The RTX 5060 vanished from Windows (no PnP device at all) on 2026-11-11 while on battery; likely
  Lenovo Vantage Hybrid-Auto / iGPU-only mode. GPU suites (M4) need AC + dGPU enabled to test it.
- 2026-11-11: the PC hard-froze once during development (Kernel-Power 41, no low-memory event logged;
  two similar resets on 11/10 predate this project). No benchmark was running then. Precautions since:
  builds use `-- -j 6`, memory arenas capped to ¼ of free RAM, no parallel heavy runs.
