# ez-pc-bench — Plan & Knowledge Base

Living doc. Update it as decisions are made or facts are learned. Keep it terse.

## 1. Goal

A small, fast, native Windows benchmark for this laptop. GUI: the user picks suites/tests in a
collapsible tree, hits Run, sees live progress + live stats of every test as the run moves through
them, and gets a results table of **raw numbers** (GB/s, GFLOPS, IOPS, ns, ms, FPS, ...). Raw numbers are
the primary output. A derived, versioned overall score (FUTURE.md §10) summarizes them; its formula, weights
and reference are published in the app and docs, and it is labelled preliminary until calibrated against
real-task timings. A CLI build runs the same benchmarks headless. Results files are TOON text (`*.toon.txt`).

Suites: **CPU**, **Memory**, **GPU** (Vulkan + Direct3D 12; Compute, 3D, 2D, Features), **Disk**
(CrystalDiskMark-style plus latency, sweeps, raw-device tests). Separate tools (own windows / CLI modes):
stability tester, drive health, sensors + log + overlay, system details, history.

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
- Release: `/O2 -march=x86-64-v2` (SSE4.2 + POPCNT) for app code. AVX2 / FMA / AVX-512 kernels per function
  (`TARGET_AVX2` in `core/types.h`, `__attribute__((target(...)))`), selected by runtime CPUID (§12 2026-11-12).
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
  app/main_gui.cpp     Win32 window, D3D11 device/swapchain, ImGui frame loop, tool-window table, dev flags
  app/main_cli.cpp     Arg parsing, runs selected tests, live progress lines, CLI modes (--health, --sysinfo, ...)
  app/ui.cpp/.h        Suite tree, live panel, Results / Charts / Score / Ranking tabs, warnings
  app/settings*.cpp    settings.ini load/save, Settings and Benchmark settings windows
  app/theme.*          Palette (graphite / cyan / orchid), outlined controls, group boxes, fonts, vector icons
  app/graph.*          Shared Task Manager-style graph widget (time series, x/y, bars, stats; colors by unit)
  app/library.*        Built-in reference runs (RCDATA, LZMS)
  app/compare_ui.*     Compare window;  app/score_ui.*, history_ui.*: score tab / About, History window
  app/snapshot.*       Window back buffer -> PNG (WIC): file (Score tab) or clipboard (compare)
  app/mem_charts.*     Memory grid table, loaded-latency plot
  app/temps_ui.*, overlay.*, adv_ui.*   Sensors window (+ log), click-through overlay, PawnIO section
  app/health_ui.*      Drive health window
  app/system_ui.*      System details window
  app/stability_ui.*, stability_cli.*   Stability tester window and `--stress`
  app/app.rc/gui.rc/.manifest           Manifest (asInvoker, PMv2 DPI, long paths), icon, fonts, library
  core/types.h         u8..u64, f32/f64, KiB/MiB/GiB
  core/bench.h         BenchParams (+ DiskOptions), BenchLive, BenchResult, Metric, SuiteDesc, BenchDesc, flags
  core/registry.cpp    The suite + test tables (only place new tests are registered)
  core/runner.cpp/.h   Worker thread: suite init → tests → suite shutdown, cancel, repeat / sustained passes
  core/results.cpp/.h  Metric arena, result helpers (res_*), TOON results file (system, params, score, integrity)
  core/toon.*          TOON reader/writer subset
  core/presets.*       Run presets (quick, standard, extended, score, sustained)
  core/monitor.*, hwread.*  Run monitor (env[] per test), driverless CPU / GPU readers
  core/sensors.*, battery.*, emi.*   Sensors sampler + channels + log, battery IOCTLs, Energy Metering Interface
  core/pawnio.*, adv_*.cpp/.h        PawnIO client, advanced sensors (AMD / Intel CPU, SMBus DIMMs, UMC timings)
  core/sysinfo.cpp/.h  CPUID, SMBIOS RAM, adapters, drives, power, OS, admin
  core/sysinfo_cpu/devices/display/gpu/win/spd.cpp, sysinfo_ext.h   CPU details, PCI/USB tree, EDID, GPU API
                       details, Windows state, SPD + timings
  core/sysreport.cpp   System report model -> window / text / TOON
  core/disk_health.*, ata.*, nvme_dev.*, scsi_pt.*, csmi.*, smart_attr.*, health_store.*, physdisk.*
                       Drive health: SMART / NVMe logs over every transport, status rules, text report
  core/compare.*, baseline.*   Compare window model, CLI --compare baseline
  core/score.*, runfile.*, history.*, integrity.*, report.*   Score v1, flat run reader, run history,
                       integrity hash, HTML report
  core/whea.*          WHEA-Logger event counter
  core/perf.cpp/.h     Process/thread perf setup (priority, throttling opt-out, timer res, keep awake)
  core/elevation.cpp/.h  is_elevated, relaunch via runas
  core/smoke_bench.cpp Smoke suite: exercises runner, live stats and all three result kinds
  core/timer.cpp/.h    QPC wrapper, rdtsc, do_not_optimize/clobber_memory
  core/threads.cpp/.h  Pinned thread pool, CPU topology (incl. EfficiencyClass)
  core/throughput.*    MT throughput driver shared by CPU and memory
  core/mem_util.*      VirtualAlloc helpers, large pages (best effort), arena
  cpu/cpu_bench.cpp, cpu_kernels.cpp (inline asm), cpu_gemm, cpu_crypto, cpu_hash (CRC32C, xxHash, AES-GCM,
      ChaCha20), cpu_compress, cpu_image (WIC), cpu_parse, cpu_sci (FFT, n-body, rays), cpu_ai, cpu_pi,
      cpu_micro, cpu_topo
  mem/mem_bench.cpp, mem_kernels.cpp, mem_grid.cpp, mem_load.cpp (loaded latency), mem_internal.h
  disk/disk_suite.cpp  Suite init/shutdown, test files / folder target;  disk.h / disk_internal.h
  disk/disk_io.cpp     Overlapped I/O engine + latency histogram
  disk/disk_cdm, disk_latency, disk_sweep, disk_sustained, disk_recovery, disk_files, disk_raw, disk_nvme
  gpu/gpu_common.*     Test table (GPU_TESTS), preview hand-off, frame-time stats
  gpu/gpu_entry.cpp, gpu_multi.cpp   Registry wrappers, suite layer (one device per selected adapter)
  gpu/gpu_vk*.cpp      Vulkan backend: base tests, _ext / _memory / _pso / _texture, _features
  gpu/gpu_d3d12*.cpp   D3D12 backend: compute, raster, _ext / _memory / _pso / _texture, _features
  gpu/gpu_ext.h, gpu_features.*      Shared definitions of the extra compute and feature tests
  gpu/gpu_present*.cpp On-screen mode: benchmark window, flip-model swapchains
  gpu/shaders/*.hlsl (+ coopmat.comp)
  stability/           Stability tester: controller + report, cpu / mem / disk / gpu (D3D12) tests, stab.hlsl
tools/pack_library.cpp, site_library.py, serve_site.ps1, make_icon.ps1
third_party/imgui/
docs/PLAN.md, FUTURE.md, notes/
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

Added in M11 (all self-verifying): Topology (`cpu.topo.percore` series per core, `cpu.topo.hybrid` per
EfficiencyClass), Compression (Windows Compression API XPRESS / XPRESS-Huffman / MSZIP / LZMS + own LZ77,
MB/s), Image (WIC JPEG / PNG decode, resize, MPix/s), Parsing (JSON, text tokenizer, MB/s), Science (FFT
GFLOPS, n-body Ginteractions/s, ray-box / ray-triangle Mtests/s), Crypto (AES-GCM via CNG, ChaCha20, CRC32C,
xxHash64, GB/s), AI inference (int8 VNNI GOPS, BF16 GFLOPS). Off by default: `cpu.work.pi` (1M digits, s),
Microarchitecture (`cpu.micro.instr` clk table, `cpu.micro.branch`, `cpu.micro.code`).

### 6.2 Memory

| Test | What | Metric |
|---|---|---|
| Read BW | AVX2 loads over 1 GiB, MT | GB/s |
| Write BW | Normal stores and non-temporal (`vmovntdq`) stores | GB/s |
| Copy BW | Normal + NT copy | GB/s |
| Triad (STREAM) | a = b + s·c | GB/s |
| Latency | Random pointer chase (shuffled cycle, one hop per cache line), 16 KiB … 1 GiB → **series** | ns |
| Cache BW sweep | Read BW per working-set size, ST + MT → **series** | GB/s |

Added in M12: `mem.grid` (L1 / L2 / L3 / DRAM read, write, copy GB/s + latency ns), `mem.bw.mix` (read:write
3:1 / 2:1 / 1:1), `mem.bw.scaling` (series over threads), `mem.lat.loaded` (latency vs generated bandwidth,
MLC-style), `mem.lat.pages` (4 KiB vs 2 MiB pages; off, needs SeLockMemoryPrivilege).

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

**2D and 3D tests render offscreen with an in-window preview** (changed 2026-11-11, was a separate window):
- Render target RGBA8 at `gpu_width × gpu_height` (default 1920×1080), no swapchain / vsync / compositor.
  ≤ 2 frames in flight; frame time = QPC delta between consecutive submissions (present-to-present
  without a swapchain).
- Preview: every ~66 ms the backend downscales the target (`cs_downscale`, ≤ 640×400), reads it back and
  calls `gpu_preview_publish()`; the UI uploads the newest frame into a D3D11 texture and shows it next to
  the live stats. A CPU hand-off is used because UI (iGPU) and test (dGPU) are different adapters.
- Each test: warm-up min(1 s, duration / 3) (discarded), then `duration_ms` of frames.
- The full contract (bindings, workloads, timing, preview) lives in `src/gpu/gpu_common.h`.
- Reported: avg / p99 / p99.9 frame time (ms), plus the derived throughput (Mtri/s, Gpix/s, ...). The
  GUI shows the equivalent fps after each frame time (`format_frame_fps`): avg = average FPS, p99 = 1% low,
  p99.9 = 0.1% low (*1% low = 1000 / p99 frame time in ms*). Frame-time history is plotted live.

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

Added in M12 (same ids on both APIs): `compute.fp64`, `compute.dp4a` (TOPS), `compute.matrix` (cooperative
matrix FP16 / INT8, Vulkan only), `compute.atomics` (Gatomics/s); `mem.latency` / `mem.sweep` (series vs
working set); `pso.pso` (PSO/s, cold vs cached); `tex.formats`, `tex.aniso` (Gtex/s); group Features:
`features.raytracing` (Grays/s, BLAS / TLAS ms), `mesh_shader`, `tessellation`, `vrs`, `sampler_feedback`
(D3D12 only), `async_compute` (overlap gain %). 3D / 2D tests can also present on screen (setting "Present
mode"); several adapters per run prefix metrics with the adapter short name.

Debug builds enable the Vulkan validation layer and the D3D12 debug layer.

### 6.4 Disk (CrystalDiskMark-style)

The default rows mirror CDM 8 "Default":
| Test | Block | Queue depth | Threads |
|---|---|---|---|
| SEQ1M Q8T1 read / write | 1 MiB | 8 | 1 |
| SEQ1M Q1T1 read / write | 1 MiB | 1 | 1 |
| RND4K Q32T1 read / write | 4 KiB | 32 | 1 |
| RND4K Q1T1 read / write | 4 KiB | 1 | 1 |
Optional NVMe extras (off by default): SEQ128K Q32T1, RND4K Q32T16. Mixed 70/30 via the "mix" mode.

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

Added in M8: p50 / p90 / p99 / p99.9 / p99.99 / max latency per pass type in every engine-based test; QD1
latency (`disk.lat.read` / `write` / `flush`, flush and FUA), `disk.lat.hist` (distribution series),
`disk.lat.load`, `disk.lat.wake`; sweeps `disk.sweep.qd` / `block` / `compress`;
`disk.fs.meta`, `disk.files.copy`, `disk.recovery`; `disk.sustained.write` and raw-device `disk.raw.span` /
`scan` / `seek` (admin, read-only handle). Modes rw / read / write / mix, folder target, buffered option.
`BENCH_CONFIRM` tests (sustained write, surface scan) ask first and are never in presets. Custom rows
(`disk.custom1..4`), CDM profiles and the all-zero test data were removed in the 2026-11-11 trim: tests
always write random data.

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

Per-stream details for M8–M12 (files, settings, formats, measurements, untested paths): [notes/](notes/).

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
- **M4 GPU infra + Compute** and **M5 GPU 3D + 2D** — ✅ done 2026-11-11, split by API: groundwork
  (contract `gpu_common.*`, 18 HLSL entry points → DXIL + SPIR-V build step, registry rows generated from
  `GPU_TESTS`, preview hand-off + UI preview, `smoke.preview` self-test) on main; D3D12
  (`gpu_d3d12*.cpp`: one RS 1.1 root signature, enhanced barriers, copy queue) and Vulkan (`gpu_vk*.cpp`:
  1.3, dynamic rendering + sync2, device by LUID, dedicated transfer queue) written by two parallel
  subagents, reviewed and merged. 14 tests per API, all pass on the RTX 5060 Laptop; compute verified on
  the 780M (adapter selection by LUID). Vulkan validation (incl. sync validation) clean; the D3D12 debug
  layer isn't installed (Windows optional feature "Graphics Tools"), so D3D12 wasn't layer-validated.
- **M6 Polish** — ✅ done 2026-11-11 (subagent in a worktree): CSV export (GUI "Save CSV", CLI `--csv`),
  settings persistence (`%APPDATA%\ez-pc-bench\settings.ini`), compare against a saved JSON (GUI
  "Compare...", CLI `--compare`), baseline deltas also in "Copy text". Details in the decision log.

- **M7 Context** (roadmap in [FUTURE.md](FUTURE.md)) — ✅ done 2026-11-11 on `dev`: run monitor (`core/monitor.*`,
  readers in `core/hwread.*`) records CPU clock / load and the test GPU's clocks, load, temperature and
  power per test (`env[]` in TOON, a short line under each result, CLI `[...]` line), with flags for clock
  drop, power-source change and hot GPU. Windows state (VBS, memory integrity, Secure Boot, TPM, Game Mode,
  HAGS per GPU), board / BIOS and GPU driver version in sysinfo. Run presets Quick / Standard / Extended and
  repeat-N with median + spread (`spread[]` in TOON). Temperatures window became Sensors (per-core clock and
  load, GPU clocks / load / VRAM / power / PCIe link).

M8–M12 were built 2026-11-11 by eleven parallel streams in worktrees (one benchmark at a time through a shared
lock), each merged into `dev`. Their measurements were taken under heavy background load (the other streams'
builds); treat CPU single-thread and L3 numbers from them as shape, not reference.

- **M8 Disk** — ✅ done 2026-11-11 on `dev` ([disk-bench](notes/disk-bench.md),
  [disk-health](notes/disk-health.md)). Benchmark: `disk/disk_bench.cpp` split into `disk/*.cpp` around one
  overlapped I/O engine (`disk_io.cpp`) with a per-thread log-bucket latency histogram (p50..max in every
  test); QD1 / flush / FUA latency, latency under load, wake-up latency, histogram series, QD / block /
  compressibility sweeps, mixed mode, metadata and copy workloads, TRIM / idle
  recovery, sustained write / SLC, raw-device span / surface scan / seek; folder target, buffered option,
  `BenchParams::disk` (`DiskOptions`), `BENCH_CONFIRM`. Health (CrystalDiskInfo parity): SMART table with
  vendor names (`core/ata.*`, `smart_attr.*`), NVMe logs / features / self-test (`nvme_dev.*`), USB bridges
  (`scsi_pt.*`), CSMI RAID members (`csmi.*`), status rules, rewritten window, text report, CLI `--health`.
  Removed again in the 2026-11-11 trim: custom rows, CDM profiles, zero test data, health history / endurance
  projection, per-drive thresholds, AAM / APM. **Untested:** raw-device tests and self-test start (need
  admin), every ATA / USB / CSMI path (no such hardware; ATA checked with a synthetic sector), sustained
  write past the SLC cache (verified at 2 GiB).
- **M9 Stability** — ✅ done 2026-11-11 on `dev` ([stability](notes/stability.md)): separate tool
  (`src/stability/`, window `--tool stability`, CLI `--stress`), tests `cpu`, `cores` (CoreCycler-style),
  `cache`, `mem`, `vram`, `gpu` (D3D12 compute + render with known answers), `power` (unchecked max load),
  `disk` (write / read-back verify); errors attributed to LP / chunk / address; sequence, together
  (`combined`) or until stopped; temperature / battery limits, stop on first error, state always restored;
  WHEA-Logger counter (`core/whea.*`); report as TOON `kind: stability`. `--inject` self-test proved
  detection end to end for every test. **Not done:** WHEA in benchmark runs, Vulkan GPU tests, settings
  persistence, CPU temperature limit from PawnIO. **Untested:** a real WHEA event (none in this PC's log).
- **M10 PawnIO sensors** — ✅ code done 2026-11-11 on `dev` ([pawnio](notes/pawnio.md)): license / HVCI /
  anti-cheat research (decision log), plain-`DeviceIoControl` client (`core/pawnio.*`), advanced sensors
  (`core/adv_sensors.*`, `adv_cpu.cpp`: AMD Tctl / Tdie / CCDs / RAPL / APERF-MPERF / SVI2 / PM table, Intel
  DTS / RAPL / limit bits; `adv_smbus.cpp`: SPD5118 / TSE2004 temperatures + SPD base config; `adv_umc.cpp`:
  AMD UMC timings), `sysinfo_spd.*` (`system.ram.timings` / `spd` in results), Sensors-window section,
  Settings row, CLI `--adv-report` / `--no-adv-sensors`, run monitor `cpu_temp` when available. **Untested:
  all of it** — PawnIO isn't installed here and no module `.bin` is shipped. Hawk Point PM table only has
  Tctl + PPT mapped. Not done: XMP / EXPO (needs an SPD page-select write, §11), Intel MCHBAR, Super I/O.
- **M11 Score + CPU kernels** — ✅ done 2026-11-11 on `dev` ([cpu](notes/cpu.md),
  [score-results](notes/score-results.md)). CPU: 26 new tests (`cpu/cpu_{topo,compress,image,parse,sci,hash,
  ai,pi,micro}.cpp`; per-core pass, hybrid split, Windows Compression API + own LZ77, WIC, JSON / text,
  FFT, n-body, rays, AES-GCM / ChaCha20 / CRC32C / xxHash, int8 / BF16, π, instruction table, branch / code
  footprint), all self-verifying; runner-level sustained mode (`sustain_s`, `sustained[]` /
  `sustain_passes[]` TOON tables, per-pass charts) and the Sustained preset. Score v1 (`core/score.*`,
  `runfile.*`): 5 components + Overall (the 3 extra profiles were removed in the trim), 11 validity flags,
  Score preset, Score / Ranking tabs, About page, compare Score group, CLI `--score` / `--score-write`; run
  history (`core/history.*`, History window), PNG (`app/snapshot.*`; compare: Copy PNG since the trim) and
  HTML report (`core/report.*`, `--html`), `integrity:` block, issue form + `site/results.html`
  (`tools/site_library.py`). **Untested / open:** `cpu.topo.hybrid` (one efficiency
  class here), the AVX-VNNI int8 path, score calibration (needs other machines). CPU ST numbers from the
  stream were taken under build load (2–5x low): re-measure on an idle machine.
- **M12 Breadth** — ✅ done 2026-11-11 on `dev` ([memory-sysinfo](notes/memory-sysinfo.md),
  [gpu-compute](notes/gpu-compute.md), [gpu-features](notes/gpu-features.md),
  [gpu-present](notes/gpu-present.md), [sensors-p2](notes/sensors-p2.md)). Memory: grid, mix, thread
  scaling, loaded latency, page sizes (`mem/mem_grid.cpp`, `mem_load.cpp`, `app/mem_charts.*`). System
  details: CPU codename / microcode / flags / core map, chipset, PCI / USB tree, displays (EDID), network,
  audio, GPU API details incl. ReBAR (`core/sysinfo_{cpu,devices,display,gpu}.cpp`, `sysreport.cpp`, window
  `--tool system`, CLI `--sysinfo`). GPU: FP64, dp4a, cooperative matrix (Vulkan), atomics, memory
  hierarchy, PSO cold / cached, texture formats / aniso (`gpu_*_ext/memory/pso/texture.cpp`), several
  adapters per run (`gpu_multi.cpp`), feature tests (`gpu_*_features.cpp`), on-screen present mode
  (`gpu_present*.cpp`); frame capture of other apps was built too and removed in the trim. Sensors:
  battery, EMI power rails, channel list with min / max / avg, sensor log (`--log-sensors`, CSV), overlay
  (`core/battery.*`, `emi.*`, `app/overlay.*`). **Untested:** `mem.lat.pages` (needs SeLockMemoryPrivilege),
  D3D12 debug layer (not installed). **Not done:** GPU memory
  type / bus width. Wired at merge time: GPU suite setup queries the GPU API details (ReBAR, feature tiers),
  the run monitor records EMI package power (`env[].cpu_pkg_w`) and WHEA events (`env[].whea`), the stability
  CPU limit uses PawnIO's temperature when present.

Each milestone: builds warning-free, runs via CLI on this machine, numbers sanity-checked (§10).

## 9. Benchmarking pitfalls (knowledge)

- **Power/thermals**: laptop boost decays over seconds; on battery or "Balanced" numbers drop. Warm-up,
  fixed duration, report best + median; keep test order stable.
- **Timers**: QPC (10 MHz) for wall time. `rdtsc` invariant on Zen 4 but runs at a fixed reference
  frequency, not core clock — short relative timing only.
- **Compiler**: scalar loops get auto-vectorized (SSE4.2 baseline, AVX2 inside `TARGET_AVX2` functions) and chains can be folded → kernels in
  inline asm; every result feeds a sink. Inspect generated asm for benchmark TUs.
- **Inline asm, equal constant inputs**: clang may put two `"r"` inputs with the same value in one
  register. `cpu.micro.branch` passed the wrap value as a separate `"r"(0)` next to a zero loop index; both
  got the same register, so incrementing the index changed the "constant" and the loop never wrapped. Make
  operands that the asm writes `"+r"` outputs, don't rely on separate inputs staying distinct.
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

## 10. Reference run (2026-11-11 11:38, AC + "Best performance", default settings, elevated)

GUI run, all 55 tests (default selection + the two NVMe disk extras), 3 s phases, 5 disk passes, BitLocker
**off** on C: from here on. Raw data: [library/2026-11-11_r7-250_rtx5060-laptop.toon.txt](../library/2026-11-11_r7-250_rtx5060-laptop.toon.txt),
built into the GUI exe (compare window > Library). Earlier runs (BitLocker on, the sieve regression) are in
git history (`docs/reference/`, removed in the library commit). Numbers are single thread → multi-thread
unless noted.

| Area | Result | Sanity check |
|---|---|---|
| Int ALU / imul | 17.5 → 137 / 4.8 → 38 Gops/s | 3.6 ops/clk @ ~4.8 GHz; 1 imul/clk |
| FP32 FMA scalar / AVX2 / AVX-512 | 19.1 → 152 / 152 → 1197 / 153 → 1217 GFLOPS | 32 FLOP/clk @ 4.76 GHz ST, ~4.7 GHz all-core |
| FP64 AVX2 / AVX-512 | 76 → 597 / 76 → 608 GFLOPS | half of FP32 |
| SGEMM 1920² | 128 → 857 GFLOPS | 84 % / 72 % of FMA peak |
| SHA-256 / AES-128 (VAES-512) | 2.4 → 34.9 / 28.3 → 217 GB/s | |
| Sort 16M / sieve 1e9 | 16.0 → 103 Melem/s / 2877 → 19756 Mnum/s | |
| Core-to-core | min 14.6 (SMT siblings) / avg 25.5 / max 70 ns | single CCX; one 70 ns outlier pair |
| FMA scaling 1/2/4/8/16 T | 152 / 304 / 609 / 1197 / 1191 GFLOPS | linear per core, SMT ~0 |
| DRAM read / write / NT write / copy / triad | 52 → 59 / 29 → 25 / 60 → 57 / 31 → 35 / 34 → 40 GB/s | read = 66 % of 89.6 peak |
| Latency L1 / L2 / L3 / DRAM | 0.8 / 3.0 / 9.7–56 / 87–119 ns (4 KiB pages) | 4 / 14 clk |
| Disk SEQ1M Q8T1 / Q1T1 | 7087 / 5984, 3669 / 5006 MB/s (read / write) | |
| Disk SEQ128K Q32T1 | 6980 / 5895 MB/s | |
| Disk RND4K Q32T1 / Q32T16 / Q1T1 | 571 / 430, 4054 / 3873 (990K / 946K IOPS), 70.7 / 209 MB/s | T16: the SSD, not one submit thread |
| GPU FP32 / FP16 / INT32 | VK 14.8 / 16.1 / 9.3, D3D12 14.7 / 17.5 / 9.2 T(FL)OPS | |
| GPU VRAM read / write / copy | VK 369 / 347 / 338, D3D12 369 / 368 / 338 GB/s | 96 % of ~384 GB/s |
| GPU LDS / PCIe | 8.7 / 8.6 TB/s / 13.8–14.0 GB/s each way | Gen4 x8 |
| GPU tri / fill / texfill | VK 2.66 Gtri/s / 108 Gpix/s / 181 Gtex/s, D3D12 2.60 / 107 / 182 | |
| GPU draw calls | VK 31.4 M/s, D3D12 67.1 M/s | driver CPU cost per draw |
| GPU scene (avg / p99 frame time) | VK 0.131 / 0.263 ms (7611 / 3798 fps), D3D12 0.140 / 0.274 ms (7165 / 3651 fps) | |
| GPU sprites / blit / clear | VK 80.5 Msprites/s / 739 / 2468 GB/s, D3D12 79.6 / 675 / 3306 | L2-resident targets, fast clears |

vs the earlier elevated run with BitLocker on: CPU / memory / GPU within ~2 % (CPU 1–2 % lower, warmer
day / chassis), but the latency-bound disk tests moved a lot — SEQ1M Q1T1 read / write 2811 / 2344 →
3669 / 5006 MB/s, RND4K Q1T1 60 / 121 → 71 / 209 MB/s, RND4K Q32T1 read 427 → 571 MB/s (write 734 → 430,
within this SSD's run-to-run swing). Software BitLocker costs the most where each I/O waits on the CPU.
**Sieve / code-placement finding:** after the TOON/multi-drive commit the sieve dropped to 1847 → 13407
Mnum/s (−33 %) although `cpu_bench.cpp.obj` disassembled identically to the old build; only the hot
loop's address in the linked exe changed. Compiling the CPU and memory kernel files with
`/clang:-falign-loops=64` (one Zen 4 op-cache line) restored 2870–2892 → 18941–19134; SGEMM, sort and the
rest were unchanged within noise. Lesson: tight loops can move ±30 % between unrelated builds — keep
loop alignment on for kernel files and compare builds, not just source.

**Background-load noise:** a re-run of the memory suite with a game client (Roblox) open in the background
gave L3-range latency of 43–106 ns and 2–16 MiB sweep reads of 24–77 GB/s, swinging 2–3x between runs
(clean run: 9–33 ns, ~125 GB/s), while its CPU usage showed only ~1 %. L3 and DRAM are shared, so close
other apps (especially anything rendering) before memory runs.

With the APIs run back-to-back after warm-up, Vulkan and D3D12 agree within ~2 % except FP16 (+9 % D3D12),
draw calls (2.1x D3D12) and blit/clear (different copy/clear paths). With the default 3 disk passes a full
run writes ~80 GB to the SSD (per pass x 3 s: SEQ1M Q8 ~18 GB, Q1 ~6 GB, RND4K Q32 ~2 GB, Q1 ~0.4 GB; 5 passes
were ~130 GB) — use 1 pass and a shorter duration for frequent runs.
## 11. Open questions

Waiting on the user (2026-11-11):
- **FUTURE.md §0 hardware scope**: which machines beyond this laptop (Intel hybrid, multi-CCD / X3D,
  > 64 LPs, NUMA, non-AVX2, ARM64), and test hardware or volunteers. §12 hardware generality and ARM64 wait.
- **§0 network**: may anything else talk to the network (email alerts, leaderboard)? Network access today:
  the PawnIO "Download & install" button (click-only) and the startup update check (`dist` builds, §12).
- **§0 resident mode**: may the GUI stay resident (tray, health / temperature alerts)?
- **PawnIO SPD page select**: allow one SMBus write to the SPD hub's page register (MR11 on SPD5118, SPA0 /
  SPA1 on DDR4), restored afterwards, to read XMP / EXPO and the DDR5 manufacturing data? (HWiNFO, CPU-Z,
  ZenTimings and Linux `spd5118` do.)
- **PawnIO module `.bin` files**: ship them (LGPL-2.1 allows it, with COPYING + a README naming the
  release), and if so embedded via `third_party/pawnio/` or as a `pawnio\` folder next to the exe?
- **Install PawnIO on this PC** to test M10 at all (driver install is the user's call; Settings > Download &
  install now does it), then map the Hawk Point PM table from an `--adv-report` dump. Also: may SMBus DIMM polling run during stability runs?
- **Admin needed** to test the raw-device disk tests and drive self-tests live.
- **Score calibration needs other machines' data**: real-task timings + Score-preset runs from 8–15
  machines (issue form) before the weights can be fitted and "preliminary" dropped.

## 12. Decision log

- 2026-11-12 (auto-update, user request): the GUI checks for updates on startup.
  - Background thread, `GET https://api.github.com/repos/0xrsp/ez-pc-bench/releases/latest` (WinHTTP, system
    proxy). Parsed with `core/json.*` (flat node array, no allocations). The `ez-pc-bench.exe` asset's `digest`
    (`sha256:...`, GitHub fills it for uploads since 2025) is compared with the running exe's SHA-256; older
    assets without one are downloaded and hashed. Different hash = update; any failure (offline, rate limit,
    no asset) = no button, nothing else changes.
  - "Update available" is the last button of the top-right tool row; it opens the release notes and "Update and
    restart" (disabled while a benchmark or stability test runs). Install: download (size, `MZ` and SHA-256
    checked), write `<exe>.new`, rename the running exe to `<exe>.old`, move `.new` into place (rolled back on
    failure), shut down normally (settings saved), start the new exe (`--no-elevate` if not elevated). Every
    start deletes leftover `.old` / `.new`.
  - Only in `dist` builds (CMake option `EZB_UPDATER`, preset `dist` = release + updater): dev builds never
    match the published exe. `--dev` / `--no-update` skip it; `--check-update` forces it for testing.

- 2026-11-12 (themes; details in [notes/themes.md](notes/themes.md)): the graphite / cyan / orchid palette below is
  replaced by plain Light / Dark (neutral grays, one blue accent) plus ten named schemes (Solarized Dark / Light,
  Monokai, Dracula, Nord, Gruvbox Dark, One Dark, Catppuccin Mocha / Latte, GitHub Light) in one static table in
  `theme.cpp` (13 colors + 9 graph hues each). ini `theme=` takes ids (`system`, `light`, `dark`, `solarized-dark`,
  ...); GUI `--theme <id>` is session only. Graph frames and grid lines use the theme's neutral colors; only the data
  line and its fill use the unit hue, taken from the current theme (`g_pal.series`).

- 2026-11-12 (look and graphs; details in [notes/theme-refresh.md](notes/theme-refresh.md),
  [notes/graphs.md](notes/graphs.md), [notes/pawnio.md](notes/pawnio.md) §10):
  - Theme replaces the Material 3 trial below: blue-black surfaces (dark page #0B0E14), cyan primary for actions and
    data, orchid secondary for navigation / selection, all text WCAG AA. Radii 2-4 px (no pills), 1 px outlines on
    frames, secondary buttons, tabs, cards and popups. `theme_group_begin/end`: titled outlined boxes, used by
    Settings, Benchmark settings, Stability, System details, Disk health, History, About the score, Sensors log tab.
  - One graph widget (`app/graph.*`) for every graph (no ImGui `PlotLines` left): Task Manager style (series-colored
    1 px frame, faint 10 x 5 grid, filled line, newest at the right, caption with title and max). Colors come from the
    unit (load cyan, clock violet, power amber, temperature rose, memory magenta, latency blue, disk green, compute
    orange), self-contained in graph.cpp and independent of the theme palette.
  - Sensors window Overview is Task Manager-like: device list with sparklines (All, CPU, GPU n, Memory, Drives, Power,
    Temperatures, Advanced) and the selected device's graphs, stats and tile grids (per-LP load, per-core power);
    span 1 / 2 / 5 / 10 min; ini `[sensors] graph_window`, `device`.
  - PawnIO modules: downloaded with the driver (or a "Download modules" button) from the PawnIO.Modules release named by
    `PAWNIO_MODULES_VERSION`, checked against `PAWNIO_MODULES_SHA256` (`core/pawnio_modules.h`), unpacked by our own
    zip reader (`core/unzip.*`) into `%LOCALAPPDATA%\ez-pc-bench\pawnio\<version>\` with COPYING; no embedded
    modules any more. The driver verifies module signatures at load. CLI `--pawnio-modules`.

- 2026-11-12 (x86-64-v2 baseline): the exes no longer need AVX2 (they used to crash at launch on pre-Haswell /
  pre-Ryzen CPUs and on Pentium / Celeron parts up to ~2021, which had AVX switched off).
  - Build: `-march=x86-64-v2 -mtune=generic` (SSE4.2 + POPCNT, the Windows 11 24H2 floor) instead of
    `/arch:AVX2`. `-mtune=generic` matters: `-march` alone also tunes for x86-64-v2 and splits unaligned 256-bit
    loads in the AVX2 functions (ray-box / ray-triangle −12 %). Both mains check SSE4.2 + POPCNT first and
    explain instead of crashing.
  - AVX2 code is per function: `TARGET_AVX2` (avx2, fma, bmi1/2, lzcnt, f16c, movbe: what `/arch:AVX2` gave),
    `TARGET_FMA`. Plain C++ kernels the compiler vectorizes are compiled twice (`FORCE_INLINE` body, AVX2 +
    baseline copies, picked by CPUID): xxHash64 (−50 % without), ray-box / ray-triangle (−50 / −65 %), FFT (also
    used by pi, −18 %), LZ77 compress / decompress (also per-core; −15 / −11 % single thread), sieve range (−3 %).
    Memory kernels have SSE2 copies (16 xmm per 256 B, triad as mulps + addps).
  - `BenchDesc::isa` (`ISA_FMA`, `ISA_AVX2` = AVX2 + FMA, `ISA_AVX512`, `ISA_AVX512_BF16`, `ISA_SHA`, `ISA_AES`)
    and `bench_unsupported(id)`: the runner skips such tests with the reason, the GUI greys them out (tooltip
    with the reason, "not supported by this CPU" in the status column, never selected by All / suite / group
    checkboxes, presets or the ini), `--list` marks them. AVX2: FP32 FMA scalar (FMA), AVX2 FMA, SGEMM, ChaCha20,
    N-body, AI INT8, thread scaling. Instruction table: ymm rows need AVX2; aesenc / pclmul rows now set up
    their registers with SSE (were VEX, would have crashed on AES-NI CPUs without AVX).
  - Stability: AVX2 and large-data kernels need AVX2 + FMA (mixed mode skips them, choosing them fails setup).
  - Dev flag `--cpu-baseline` (GUI and CLI): clears AVX / FMA / AVX-512 in `g_sys.cpu` after `sysinfo_init` to
    try the gated tests and SSE fallbacks on a modern PC.
  - Checked: no VEX instruction outside gated functions (llvm-objdump over all objects). Interleaved A/B/A/B
    against the `/arch:AVX2` build on this laptop (on battery, 1 s phases): all CPU / memory results within
    run-to-run noise (memory latency / sweeps and Windows codecs vary ±10-20 % between runs of the same exe).
    `--cpu-baseline` run: gated tests skip with the reason, the rest (SSE memory kernels, baseline copies)
    pass their self-checks.
  - Not done: the score needs `cpu.fp.scalar` and `cpu.fp.avx2_f32`, so CPUs without FMA / AVX2 only get
    component scores ("incomplete"). Needs a score-version decision.

- 2026-11-11 (trim): features cut, UI reworked (details in [notes/](notes/), marked "Removed 2026-11-11 (trim)"):
  - Disk health: History and "Thresholds & features" tabs gone (health samples, plots, endurance projection,
    per-drive thresholds, AAM / APM). Fixed CrystalDiskInfo-style Caution limits: any reallocated / pending /
    uncorrectable sector or NVMe media error, or ≤ 10 % life left. `health_store.*` keeps only `[ui] hide_serial`.
  - Disk benchmark: no custom rows (`disk.custom1..4`, `--disk-custom`, ini `disk_custom1..4`,
    `params.disk_custom`), no CDM profiles (`--disk-profile`, GUI combo), no test-data choice (`--disk-data`,
    ini / `params.disk_data`): tests always write random data. The compressibility sweep stays.
  - Frame capture removed (ETW, Frames window, `--capture`, `--capture-etl`, `--capture-selftest`, compare
    frame graph); compare accepts only `kind: benchmark` (or no kind). The GPU on-screen present mode stays.
  - Compare: Save PNG → Copy PNG (clipboard: PNG + DIB); the Score tab keeps Save PNG (file).
  - Score: Gaming / Creator / Everyday profiles removed; Overall (30/20/15/20/15) and the five components
    unchanged, still v1. `score:` no longer writes `profiles[]`; older files with it read fine.
  - Sensors: log is CSV only (`--log-sensors` keeps the given name); search box on Overview / Sensors (Ctrl+F,
    Esc; dev flag `--sensor-filter`); CPU package / per-core rails in the CPU section (and right after the CPU
    clocks in channel / CSV order), Power keeps battery and non-CPU rails.
  - UI: Benchmark settings window (`--tool bench`, sliders button next to Run) holds the preset-driven values
    (duration, repeat, sustained mode, disk file size, passes, interval); Settings (gear) keeps the rest; same
    ini keys. GPU combo: theme checkboxes, compact popup, VRAM column (`--open-gpu-combo`). Base font 15 → 16.5 px.
  - PawnIO "Download & install" (user request): `URLDownloadToFileW` of the PawnIO.Setup latest-release
    `PawnIO_setup.exe` to %TEMP%, Authenticode check (signer "namazso"), run (UAC from its manifest), delete,
    re-detect. The app's first network access, only on that click; `--adv-report` prints the installer URL.
- 2026-11-11 (M11 score v1, [notes](notes/score-results.md)):
  - ratio = value / ref (ref / value when lower is better); component = 1000 · weighted geometric mean of
    its input ratios; Overall = weighted geometric mean of the components (30/20/15/20/15; the Gaming /
    Creator / Everyday profiles were removed in the trim). Inputs, weights and references are one static
    table per version (`s_v1_inputs`, `core/score.cpp`), so adding library runs never changes a score; a new
    version is a new table and scores of different versions are never compared (the version is stored with
    every score).
  - Reference = the library run `2026-11-11_r7-250_rtx5060-laptop` = 1000 in every component and Overall.
    GPU inputs take the better of Vulkan / D3D12; storage uses C: (else the first tested drive). Missing
    inputs: component from what ran (coverage shown), no Overall.
  - Labelled **preliminary** (UI, CLI, TOON `status`, website) until calibrated against real-task timings
    (method in the notes, §11). Validity problems are 11 flags shown next to the score, never hidden.
  - `score:` block at the end of every results file (derived, recomputed by every reader; `--score-write`
    adds it to old files); `integrity:` block hashes everything except `score` / `integrity` (weak by
    design). Runs auto-save to `<exe dir>\results\history\` (CLI `--no-history`).
  - Rule reworded (CLAUDE.md, §1): raw numbers stay primary; the score is the only aggregate.
- 2026-11-11 (M10 PawnIO, [notes](notes/pawnio.md)):
  - License: our client is an independent program talking to the driver only through `DeviceIoControl`,
    which the driver's GPL-2.0 special exception allows; we don't use PawnIOLib (LGPL) and never ship or
    install the driver (users get it from pawnio.eu; since the trim, Settings' Download & install fetches and
    starts the official signed installer on the user's click). The signed module `.bin` files are LGPL-2.1 and may be
    shipped unmodified with COPYING + a README naming the release (as LibreHardwareMonitor does); not shipped
    yet (§11). Not legal advice.
  - HVCI: no official statement; very likely compatible (in-driver bytecode interpreter, no code generation;
    a user report with memory integrity on). Untested here.
  - Anti-cheat: FACEIT blocked `PawnIO.sys` in 2025 (signing certificate shared with cheat software),
    re-signed and fixed as of PawnIO 2.1.0; no reports for Vanguard / EAC / BattlEye / Javelin. Our switch
    only closes our handles: users who must satisfy an anti-cheat have to uninstall PawnIO.
  - Read-only: we only call read functions (`ioctl_read_msr`, `ioctl_read_smn`, PM table, `ioctl_smbus_xfer`
    reads); never `ioctl_write_*`, SMU commands, PIIX4 port select, clock setting, SPD page select, Super I/O
    port writes. Bus mutexes `Global\Access_PCI` / `Global\Access_SMBUS.HTP.Method` around every batch.
  - Polling: one TIME_CRITICAL 1 Hz thread, PM table ≤ 1 Hz; while a benchmark runs only package-scope reads
    (no per-LP hopping, no SMBus); any SMBus timeout / collision stops SMBus for the session (EC-owned bus).
    `adv_sensors=1` by default; everything falls back to driverless sources without PawnIO.
- 2026-11-11 (M11 sustained mode, [notes](notes/cpu.md)): runner-level, any test: loops a test for
  `sustain_s` (≥ 2 passes, ≤ 500), reports the median over passes with min / max / CV in `spread[]`, per-pass
  values in `sustained[]` / `sustain_passes[]`. **Sustained mode overrides repeat.** `drop_pct` > 0 = slower
  at the end (inverted for lower-is-better units).
- 2026-11-11 (M12 multi-adapter, [notes](notes/gpu-compute.md)): `BenchParams::gpu_adapters` bitmask; the
  primary adapter (monitor, save name, `params.gpu`) is the lowest selected. One adapter: metric names
  unchanged; several: `<short name>: <metric>` (`rtx5060-laptop: Throughput`, duplicates `#<index>`). A
  failure on any adapter fails the test, a skip on all skips it. ini `gpus=A;B`, TOON `params.gpus: A + B`
  only when several.
- 2026-11-11 (file kinds): non-benchmark TOON files carry a top-level `kind:` — `stability` (stability
  report). Benchmark results have no `kind` (= `benchmark`). The compare window accepts `benchmark` only;
  History skips any other kind. (`frames` and `sensor-log` went with frame capture and the TOON sensor log
  in the trim.)
- 2026-11-11 (M8 disk):
  - `BENCH_CONFIRM` flag for heavy tests (sustained write, surface scan): the GUI asks first with a dialog
    listing them, the CLI drops them unless `--yes`, presets (Extended included) never select them.
  - Percentiles p50..max in every engine-based test; the full distribution is its own series test
    (`disk.lat.hist`, fixed 61-point grid) because a result has one kind. Fixed x grids (also the sustained
    write's 100 GB marks) so repeats and compares line up.
  - Requests still in flight at the end of a pass are drained for up to 1 s and their latency recorded.
  - Sustained write uses 4 files in parallel (extending writes are synchronous on NTFS; `SetFileValidData`
    needs a privilege and exposes stale data).
  - The test file is held without `FILE_SHARE_DELETE`: POSIX-semantics `DeleteFileW` let a second instance's
    stale-file cleanup unlink it under a running test.
  - Raw-device tests open `\\.\PhysicalDriveN` with `GENERIC_READ` only. The display unit only changes the
    GUI, files keep every number.
  - Health: status rules CDI-like (Bad / Caution; fixed limits since the trim, which dropped the per-drive
    thresholds, AAM / APM and history); self-tests only from a button + confirmation, admin only; USB vendor
    CDBs only for their own VID, SAT for the rest.
- 2026-11-11 (M9 stability): own tool, no shared path with the runner; GPU tests D3D12 only. Workers at
  normal priority (HIGHEST starves the OS sensor threads, below-normal starved under other load), 1 Hz
  monitor at HIGHEST. Verdict `fail` also on any WHEA event during the run (corrected ones included). Memory
  test leaves ≥ 2 GiB available (default 70 % of available), VRAM 85 % of the free budget. Known answers
  from optimized scalar code (`_mm_*_ss`; `optnone` references made setup take 8–15 s).
- 2026-11-11 (M12 on-screen present mode, [notes](notes/gpu-present.md)): `gpu_onscreen` (default off =
  offscreen as before) + `gpu_fullscreen` (borderless vs window); setting "Present mode", ini
  `gpu_onscreen`, CLI / GUI `--gpu-present`, TOON `params.gpu_present`. Same rendering as offscreen, then a
  copy into a flip-model swapchain, vsync off with tearing; frame time stays present-to-present, plus
  displayed-frame stats (D3D12 frame statistics on vblank change, Vulkan present_wait). The benchmark window
  lives on the worker thread; closing it fails that test. Offscreen stays the mode for throughput numbers.
- 2026-11-11 (M12 frame capture, removed again in the trim): ETW real-time session with the DXGI and
  DxgKrnl manifest providers, filtered to their present event ids, payloads decoded by offset (no TDH); frame time = Present_Start
  delta (PresentMon's MsBetweenPresents); no displayed-frame reconstruction. Needs admin or Performance Log
  Users.
- 2026-11-11 (M12 system details): three cost tiers — `g_cpux` at startup, `g_dev` (PnP walk) lazily,
  `g_gpux` (Vulkan instance + D3D12 device) only on request because it wakes a sleeping dGPU. No MAC / IP
  addresses. `system.details` in results files; compare ignores it.
- 2026-11-11 (M12 memory / sensors): `mem_init` raises the working-set minimum by the arena + 64 MiB (trimmed
  pages made a 1 GiB chase read 1.4 µs under load). Loaded latency's x = delay cycles, not measured
  bandwidth, so runs align. `sensors_start` is reference counted (window, log, overlay); thermal zones and
  the battery are polled on their own threads. TOON `env` gained `bat_discharge_w` (appended, 16 columns);
  `TOON_MAX_FIELDS` was raised from 16 to 24 when the streams were merged.
- 2026-11-11 (M12 GPU compute): D3D12 cooperative matrix skipped (no matrix HLSL in the released DXC; SM 6.9
  cooperative vectors need the preview Agility SDK = new dependency). Cold PSOs get a new seed per pipeline:
  Vulkan specialization constant, D3D12 by patching the seed literal in the DXIL and recomputing the
  container checksum (changing only the hash part hit NVIDIA's cache).
- 2026-11-11 (branches): day-to-day work on `dev`; `main` only gets finished work via `git merge --no-ff dev`.
- 2026-11-11 (M7 run monitor):
  - Sampled every 250 ms on a TIME_CRITICAL thread. CPU load from `SystemProcessorPerformanceInformation`; CPU
    clock = utilization-weighted "% Processor Performance" x nominal clock over LPs ≥ 50 % busy. Averages are
    time-weighted (samples get further apart under all-core load). The first sample of each test and of each
    phase is skipped (rate counters span the previous one); clock drop is measured per phase (single-thread
    boosts higher than all-core on purpose) and needs ≥ 9 samples.
  - No CPU temperature: the ACPI zone stalls under load and doesn't follow the CPU here (TZ01 sat at 32 °C).
  - Repeats run each test N times in a row (suite init once); the result keeps the first run's layout with
    the median value, min / max / CV go to `g_spread`. Extended preset uses one disk pass per run, so a
    median of 3 replaces the best of 3 at the same SSD wear.

- 2026-11-11 (roadmap, details in [FUTURE.md](FUTURE.md)): kernel-level sensors (per-core CPU and RAM
  temperatures, package power, SPD, memory timings) will use the existing signed **PawnIO** driver, opt-in
  and never bundled-installed by us; no WinRing0 or other blocklisted drivers. Stability testing (CPU, memory
  error test, VRAM test, GPU error detection, WHEA) becomes its own tool, separate from benchmarks. An
  **overall score** derived from the raw results (versioned formula, component breakdown, weights calibrated
  against real-task timings) will be added; raw numbers stay primary and the "no scores" rule gets reworded
  when it's built. Cinebench-style CPU rendering and a Heaven-style heavy GPU scene are deferred.

- 2026-11-11 (tool windows, temperatures, disk health):
  - Compare, Settings, Temperatures and Disk health are "tool windows" (main_gui.cpp `s_tool_desc`): each its
    own HWND + swapchain on the shared device + ImGui context; asking for an open one focuses it. Buttons sit
    right-aligned on the System header row (vector-drawn icons, Inter has none); Settings left the side panel
    for its own window, every row with a detailed hover description.
  - Temperatures (`core/sensors.*`, 1 Hz background thread while the window is open): ACPI thermal zones via
    the "Thermal Zone Information" PDH counters (no admin; on this laptop one zone, TZ01); GPU temperature /
    fan / power via D3DKMT adapter perf data (what Task Manager uses; both the RTX 5060 and the 780M report);
    drive temperature via StorageDeviceTemperatureProperty. Per-core CPU and RAM temperatures need a kernel
    driver (MSR / SMBus) — deliberately not used. Drive polling pauses during disk tests.
  - Disk health (`core/disk_health.*`): NVMe Health Information log (page 2) via
    IOCTL_STORAGE_QUERY_PROPERTY / ProtocolTypeNvme; other buses fall back to IOCTL_STORAGE_PREDICT_FAILURE.
    The Samsung NVMe answers the log query on a query-only handle, i.e. without admin rights.

- 2026-11-11 (run library, save names, 3 disk passes):
  - Reference runs live in `library/*.toon.txt`. At build time `tools/pack_library.cpp` (`ezb_pack`, a host
    exe built in the same tree) concatenates them ("EZBL" v1: count, then name + text per run) and
    compresses the whole blob with the Windows Compression API (LZMS, Cabinet.dll — part of Windows, no
    new dependency); the GUI embeds it as RCDATA 103 via a generated `library.rc`. One run: 30.9 KB ->
    6.7 KB; runs share most of their text, so the ratio improves as the library grows. `app/library.*`
    decompresses on first use; the compare window's "Library..." lists the runs (name, CPU, GPU, RAM,
    power, date via `compare_peek`) and adds the ticked ones (`compare_add_buffer`). The CLI has no library.
  - "Save results..." opens a dialog for an optional run name; Enter keeps the default
    `ezbench_<date>_<time>_<cpu>_<gpu>`, with short names from `cpu_short_name` / `gpu_short_name`
    (sysinfo: "r7-250", "i7-13700h", "rtx5060-laptop", "radeon-780m", "rx7900-xtx"). The name is the
    file name (sanitized, `_2`... if it exists). "Copy text" and `results_format_text` were removed.
  - Default disk passes 5 -> 3 (best of 3): ~80 GB written per full run instead of ~130 GB.

- 2026-11-11 (frame times + compare summary): GPU frame tests report frame times only (avg / p99 /
  p99.9 ms); the FPS rows were exact reciprocals. The GUI (tree, results, compare) shows the fps in
  parentheses after frame-time values. The reference files had their fps rows removed (494 metrics).
  Compare window gained a summary above System: overall and per-suite geometric mean of performance
  ratios vs the reference (ratios flipped for lower-is-better units, so + = faster), faster / slower /
  same counts (1% threshold), largest gain / loss with the metric on hover. Left out of the summary:
  unit x / %, matrix min / max, "Theoretical" constants and p99.9 frame times — those swung 2–8x
  between two identical-settings runs (a few hitch frames) and pulled the Vulkan average from −2% to −12%.

- 2026-11-11 (compare window): "Compare..." opens a second top-level window (own HWND, swapchain on the
  shared D3D11 device, own ImGui context and font atlas; `g_fonts` is swapped per window, the Win32
  backend gets each window's io explicitly via `ImGui_ImplWin32_WndProcHandlerEx`). `core/compare.*`
  aligns up to 16 runs (TOON files and/or the current in-memory run) into rows keyed by (test, metric,
  x); matrices reduce to off-diagonal min / avg / max rows. The UI shows system info, raw values and
  the change vs a chosen reference run, with suite tabs, search, drag & drop (UIPI filter opened for
  elevated runs) and a tab-separated "Copy table". The single-baseline Δ columns in the main results
  table were removed; the CLI keeps `--compare <file>`. GUI `--compare [files...]` opens the window.
  Project licensed MIT; releases ship the two exes as separate assets (no zip).

- 2026-11-11 (reference + build): CPU and memory kernel files are compiled with `/clang:-falign-loops=64`
  so hot-loop placement can't swing results between builds (sieve −33 %, see §10). README.md added
  (build, run, results format, reference numbers); the elevated run is the reference baseline.
- 2026-11-11 (results format + targets):
  - JSON and CSV export removed; the only results file is TOON text, `*.toon.txt`, written and read by our
    own ~350-line subset (`core/toon.*`): `key: value`, nested objects by 2-space indent, tabular arrays
    `name[N]{fields}:` with comma rows, inline arrays, quoting only when needed. Layout: system / params
    objects + flat `tests[N]{...}` and `metrics[M]{id,name,unit,value,x}` tables (x = series x / matrix cell
    / 0). The reference run is 34 KB as TOON vs 51 KB JSON / 50 KB CSV. GUI "Save results"; CLI `--out`,
    `--compare <file.toon.txt>`.
  - Disk suite runs on several drives: `BenchParams::disk_drives` bitmask (CLI `--disk-drives C,K`, ini
    `disk_drives=C,K`); one test file per drive, metrics named per drive (`C: Read`, `K: Read IOPS`, ...).
  - GPU and disk targets moved from Settings to a bar above the toolbar (GPU combo, drive toggle chips).
  - Smoke-test suite hidden in the GUI unless `--dev` (or `--only`); hidden suites never run.
  - Scrolling panels nest the scroll region inside the rounded card so the scrollbar stays off the corners.
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
- 2026-11-11 (UI theme, trial; superseded 2026-11-12): Material 3 look in `app/theme.*` — light (default) + dark palettes from
  seed #0B57D0, flat page + white outlined cards, pill buttons, Material checkboxes (`ImGuiCol_CheckboxSelectedBg`
  is the checked fill in 1.93), segmented Results/Charts tabs, data table with horizontal dividers only.
  Font: Inter 4.1 Regular/Medium embedded as RCDATA (exe 846 KB → 1.7 MB), rasterized sharp: OversampleH/V 1,
  PixelSnapH, font sizes snapped to whole pixels after DPI scaling. Dark mode toggle in Settings
  (`--dark` flag for screenshots). Replaced the earlier violet/starfield theme at the user's request.
  Follow-up: theme mode System (default, reads `AppsUseLightTheme`, live via WM_SETTINGCHANGE
  "ImmersiveColorSet") / Light / Dark; `--light` / `--dark` override. Result numbers in Cascadia Mono
  (system font, 0.92x size), right-aligned in the results table. Warning card auto-sizes (AutoResizeY).
- 2026-11-11 (M0):
  - Disk target is a drive letter (`BenchParams::disk_drive`); the file path is derived per §6.4.
  - `BenchLive` carries `phase`/`unit` as static-string atomics instead of a phase index table.
  - Smoke suite kept as a permanent runner self-test (`smoke.*`).
  - GUI verification: launch with `--no-elevate --autorun` and capture the window with `PrintWindow`.
- 2026-11-11 (M6):
  - CSV: `suite,group,test,id,status,metric,x,value,unit`, one row per metric, UTF-8 with BOM (Excel reads
    "µs"), RFC 4180 quoting, CRLF. Series fill `x` (raw, e.g. bytes); matrix cells are `Name [row,col]`;
    failed/skipped/cancelled tests get one row with the reason in `metric`.
  - Settings ini via the profile API, file created with a UTF-16 BOM so it is written as Unicode.
    `[settings]` theme (system/light/dark), duration_ms, threads, gpu (adapter *name*: indices can change),
    gpu_width, gpu_height, gpu_fullscreen, disk_drive, disk_file_mib, disk_passes, disk_write_through;
    `[tests]` `<cli_id>=0|1` (unknown ids kept on save, ignored on load; missing ids keep the default).
    Saved 1 s after the last change (UI thread, not during a run) and at exit. `--duration`, `--light`,
    `--dark` apply to the session only; the ini keeps its value unless the user changes that setting.
  - Baseline (`core/baseline.*`): hand-written JSON reader for our own export format, results kept in a
    fixed table + open-addressing hash keyed by (test id, metric name, x); x = series x, matrix cell
    index, 0 for plain values (matched by name). Δ = (value − base) / |base| in %. Units ns, µs, us, ms, s
    are lower-is-better, all others higher-is-better; |Δ| < 1% is shown dim. Matrices compare the
    off-diagonal average.

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
- GPU, RTX 5060 Laptop on AC, both APIs on one build, 1.5 s phases (Vulkan first, D3D12 second):
  | Test | Vulkan | D3D12 |
  |---|---|---|
  | FP32 / FP16 / INT32 | 13.7 / 15.6 TFLOPS / 9.2 TIOPS | 12.2 / 15.3 / 8.4 |
  | VRAM read | 364 GB/s | 346 GB/s (128-bit GDDR7, ~384 theoretical) |
  | LDS | 8.7 TB/s | 8.6 TB/s |
  | Upload / readback | 14.1 GB/s | 14.2 GB/s (PCIe Gen4 x8 link, ~15.75 theoretical) |
  | Triangles / fill / texfill | 2.95 Gtri/s, 104 Gpix/s, 180 Gtex/s | 2.68, 94, 150 |
  | Draw calls | 30.0 M/s | 39–67 M/s |
  | Scene / sprites | 7000 fps / 67.8 Msprites/s | 6180 / 65.7 |
  | Blit / clear | 678 / 2321 GB/s | 608 / 2408 GB/s |
  Blit / clear exceed VRAM bandwidth: two 1080p RGBA8 targets (16.6 MB) fit in the 32 MB L2, clears are
  fast-cleared. The later suite in a run is ~5–15 % slower (heat / boost decay) — compare APIs run-swapped.
  Radeon 780M compute: FP32 5.2–5.8 TFLOPS, INT32 ~0.9 TIOPS (32-bit imul is slow on RDNA3), VRAM read
  ~84 GB/s (shared DDR5, 89.6 theoretical), transfer ~35 GB/s (RAM to RAM).
- Draw calls are CPU-bound in the driver: Vulkan records 10k (push constant + draw) at ~30 ns/draw vs
  ~13 ns in D3D12 (both measured around the record loop). Push-constant ranges declared for
  `VK_SHADER_STAGE_ALL` made NVIDIA's Vulkan path ~40 % slower (22 → 30.5 M draws/s): ranges now use
  compute|vertex|fragment (`VK_PC_STAGES`). Device-level `vkGetDeviceProcAddr` pointers (no loader
  trampoline) are used for the hot loop but made no measurable difference.
- NVIDIA places write-combined host heaps (D3D12 UPLOAD and custom L0 + write-combine) in VRAM behind
  ReBAR, so "upload" from them measured 38–160 GB/s (a VRAM copy). Transfer tests use CPU-cached system
  memory (D3D12 custom heap L0 + write-back; Vulkan HOST_VISIBLE without DEVICE_LOCAL).
- Vulkan FP16: DXC's SPIR-V for `cs_fp16` also declares the Int16 capability → needs shaderFloat16 AND
  shaderInt16. NVIDIA exposes a dedicated transfer-only queue family.
- Shared shader bugs found by the backend agents and fixed on main: `vs_scene` pitched the camera up (empty
  frame), `vs_drawcall` used the draw index as the grid column count (`% 0` on draw 0).
- Driverless sensor timing with all 16 LPs busy at HIGHEST in HIGH_PRIORITY_CLASS (2026-11-11): PDH
  "Processor Information" 0.1 ms idle → 120–260 ms; "Thermal Zone Information" → 5–20 **s**; the processor
  performance-distribution query (class 100) ~230 ms; "GPU Engine" stays 0.5 ms; `SystemProcessorPerformance-
  Information` 0.1–80 ms. They're served by OS worker threads that starve behind the benchmark threads.
- Measuring the clock with our own dependent-add chain timed by the TSC is wrong under SMT load: with the
  sibling running the imul test it read 1.2 GHz while throughput implied 2.45 GHz (int ALU sibling: correct).
- D3DKMT node perf data: the 780M always reports core clock (0.8 GHz idle, max 2.7); the RTX 5060 Laptop
  reports 0 when idle and ~2.35–2.6 GHz under load, memory 9.0 GHz (max 12.0), no power figure. PCIe:
  RTX 5060 runs Gen 4 x8 (device capable of Gen 5 x16). NVIDIA driver is now 32.0.15.9649.
- 2026-11-11: the PC hard-froze once during development (Kernel-Power 41, no low-memory event logged;
  two similar resets on 11/10 predate this project). No benchmark was running then. Precautions since:
  builds use `-- -j 6`, memory arenas capped to ¼ of free RAM, no parallel heavy runs.

M8–M12 streams (2026-11-11, AC + "Best performance", not elevated, mostly under the other streams' build
load; details and full tables in [notes/](notes/)):
- Query stalls under all-core load: `IOCTL_BATTERY_QUERY_STATUS` 0.02–1.7 ms idle → **2.4 s**; PDH
  "% Processor Performance" up to 2.1 s; one sensor sample took 3157 ms (battery 2367 + PDH 556 + EMI 213).
  Battery, thermal zones and PCIe link now poll on their own threads / slower cadence.
- With stability workers at normal priority the ACPI zone did follow load (52 °C idle → 64–74 °C); it
  still isn't a die temperature. D3DKMT reports no power for the RTX 5060 Laptop.
- Disk, C: Samsung PM9C1a: QD1 4K read avg 58.7 µs (p99 112, p99.99 461), write 21 µs. 4K write +
  `FlushFileBuffers` **544 µs**; with FUA (write-through) 21 µs, same as a plain write — the flush costs, FUA
  is free on this drive / StorNVMe.
- QD1 read next to SEQ1M Q8 writes: p99 678 µs, p99.9 3.8 ms (idle p99 131 µs; possibly disturbed run).
  Wake-up after 1 s idle: median 1.67 ms (max 10 ms); PS3 / PS4 exit latency 9 / 44 ms.
- RND4K Q32T4 write torture cut sequential reads 7x (5.8 → 0.85 GB/s); 30 s idle restored 5.5 GB/s (GC).
  `FSCTL_FILE_LEVEL_TRIM` fails on C: with error 326. Compressibility (0–100 % zeros) has no effect.
- QD sweep, 4K random read: 15.4K IOPS QD1 → 133K QD16 (one thread tops out ~130K) → 788K QD256 (8 threads).
  Sustained write 2 GiB: 4974 MB/s average, no SLC drop (cache is larger). Metadata: file create 302/s, p50
  1.25 ms, mostly Defender's on-access scan.
- NVMe without admin, on a query-only handle: Identify, logs 1/2/3/6, most Get Features, temperatures and
  partition layout work; opening with GENERIC_READ fails, so pass-through, SMART IOCTLs and self-tests need
  admin. The drive is DRAM-less (64 MiB HMB), PCIe 4.0 x4.
- Memory: one core already pulls 43 GB/s, two saturate the fabric (54–60 GB/s). Loaded latency 127 ns at
  0.06 GB/s → 196 ns at 34 GB/s → 688 ns at 55 GB/s. SeLockMemoryPrivilege isn't granted even to admins.
- Zen 4 instruction table matches published figures (add 1, imul 3, FMA ymm 4 clk; zmm at half rate).
  4-byte NOPs run ~12 / clk up to 32 KiB of code, ~3.9 beyond the op cache, 2.0 at 2 MiB. int8 VNNI ~108
  ops/clk ST (85 % of peak). π 1M digits 1.66 s ST / 0.645 s MT.
- CPU ST numbers measured next to parallel compiles were 2–5x low and some MT / ST ratios exceeded 16; the
  score's `background` flag fired (mem.lat.chase at 29 % CPU vs ~6 % alone).
- **DXC `[unroll(N)]`**: DXIL treats N as "at most N iterations" (D3D12 took 8 of 128 texture samples,
  14x too fast); SPIR-V treats it as a hint. Don't use `unroll(N)` on loops longer than N.
- **NVIDIA PSO cache**: changing only the DXIL hash + checksum still hit the driver cache (~1.2 ms vs ~35
  ms cold); the shader body must change. Cold PSOs take 25–45 ms on both GPUs and APIs (a visible hitch at
  60 fps), cached ones are 100–2000x faster; NVIDIA D3D12 pipeline libraries load ~6x slower than its Vulkan
  pipeline cache.
- **Tessellation**: RTX 5060 D3D12 1.06 ms (17.8 Gtri/s) vs Vulkan 2.87 ms (6.6 Gtri/s) for the same HLSL;
  the 780M is equal. Likely DXC's hull-shader → TCS lowering on NVIDIA's Vulkan path; not investigated.
- dp4a: D3D12 1.5x (NVIDIA) / 1.7x (AMD) faster than the same HLSL via SPIR-V `OpSDot`. RTX 5060: FP64 =
  1/64 of FP32 (0.24 TFLOPS); cooperative matrix 35.9 TFLOPS FP16 / 133 TOPS INT8 (Vulkan). One-address
  atomics are aggregated per wave (76 G/s into one word).
- RTX 5060 memory hierarchy: L1 ~18 ns to 64 KiB, L2 ~117 ns to 32 MiB, VRAM ~340 ns. 780M: steps at 16 KiB,
  ~256 KiB, 2 MiB, then shared DDR5 at 200–300 ns / ~80 GB/s.
- Features, RTX 5060: 4.6 Grays/s per frame (1.45 ms with BLAS refit + TLAS rebuild); mesh shaders +146 %
  (VK) / +236 % (D3D12) over the vertex pipeline; async compute +75 % — but ~0 with a pixel-shaded graphics
  load (no free shader slots), so the test's graphics part is depth-only. Sampler feedback on every pixel:
  +319 % (RTX 5060), 16x (780M). 780M terrain BLAS full build 12 ms vs 0.6 ms.
- On-screen present adds ~0.35–0.45 ms per frame on **both** GPUs (not just the dGPU's cross-adapter copy),
  capping on-screen runs near 1400–1700 fps. D3D12 frame statistics must be logged on `SyncQPCTime` change
  (per-present logging gave "95 % displayed"); NVIDIA present_wait completes ~3 ms after the call, not at
  scan-out.
- ETW (frame capture, removed in the trim): `StartTraceW` returns ERROR_ACCESS_DENIED without admin or
  Performance Log Users.
- Battery: design 60 Wh, full 60 Wh, 59 cycles; −20.5 W at desktop load on battery; on AC at 42 % it reports
  charging at 0 W (conservation mode). EMI: 9 meters (package + 8 cores) readable without admin; package
  holds ~39 W under all-core load on AC (platform limit).
- VRAM budget with nothing else on the dGPU: 7119 MiB of 8 GiB.
