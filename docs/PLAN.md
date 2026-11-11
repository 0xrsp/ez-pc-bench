# ez-pc-bench — Plan & Knowledge Base

Living doc. Update it as decisions are made or facts are learned. Keep it terse.

## 1. Goal

A small, fast, native Windows GUI benchmark for this laptop. The user picks suites/tests in a
collapsible tree, hits Run, sees live progress + live stats while each test runs, and gets a
results table of **raw throughput numbers** (GB/s, GFLOPS, IOPS, ns, Mtri/s, ...) at the end.
No synthetic "scores".

Suites: **CPU**, **Memory**, **GPU** (Vulkan + Direct3D; 2D, 3D, Compute), **Disk** (CrystalDiskMark-style).

Scope: only needs to work on this machine for now. No installer, no cross-platform.

## 2. Target machine (measured 2026-11-11)

| Item | Value |
|---|---|
| CPU | AMD Ryzen 7 250 (Zen 4 "Hawk Point"), 8C/16T. AVX2 + AVX-512 (Zen 4 = 256-bit datapath, double-pumped) |
| Caches (expected for Zen 4 mobile) | L1D 32 KiB/core, L2 1 MiB/core, L3 16 MiB shared — verify via CPUID at runtime, don't hardcode |
| RAM | 31.3 GiB usable |
| dGPU | NVIDIA GeForce RTX 5060 Laptop (Blackwell), driver 32.0.16.2114 |
| iGPU | AMD Radeon 780M (RDNA3) |
| Disk | Samsung MZAL81T0HFLB 1 TB NVMe (C:, NTFS) |
| K: drive | 250 GB **ReFS Dev Drive backed by a VHD file on C:** — repo lives here. Do NOT use as default disk-bench target (measures VHD + ReFS overhead) |
| OS | Windows 11 Pro 10.0.26300 |

Hybrid graphics: two adapters. GPU tests must enumerate and let the user pick (default: the
discrete NVIDIA). Laptop: power source and thermals affect results (see §9).

## 3. Toolchain (measured 2026-11-11)

| Tool | Location / version |
|---|---|
| clang / clang-cl | 23.1.5, standalone LLVM at `C:\software\clang+llvm-23.1.5-x86_64-pc-windows-msvc\bin` (on PATH). Target `x86_64-pc-windows-msvc` |
| CMake | 4.5.0-rc3 (`C:\Program Files\CMake\bin`) — generators incl. Ninja, VS 18 2026 |
| MSVC | VS 2026 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`), tools 14.52.37025 |
| VS clang-cl toolset (`VC\Tools\Llvm`) | **Not installed** → `-T ClangCL` with VS generator won't work out of the box |
| Windows SDK | 10.0.26100.0 (has `dxc.exe`, `fxc.exe` in `bin\10.0.26100.0\x64`) |
| Vulkan SDK | 1.4.363.1 at `C:\VulkanSDK\1.4.363.1`; `VULKAN_SDK` is set machine-wide (may need a new shell to see it). Has `dxc.exe`, `glslc.exe` |
| Ninja | **Not installed.** Install: `winget install Ninja-build.Ninja` |
| git | 2.56.1 |

### Build approach (decided, pending Ninja install)

- Generator: **Ninja**, compiler **clang-cl** (MSVC ABI, links against MSVC CRT + Windows SDK), linker `lld-link`.
- Run CMake from a VS x64 dev environment (`vcvars64.bat` / "x64 Native Tools" prompt) so INCLUDE/LIB
  point at MSVC + SDK. clang-cl can autodetect them, but vcvars is the robust path (also gives `rc.exe`, `mt.exe`).
- `CMakePresets.json` with `debug` and `release` presets (`build/debug`, `build/release`).
- Release flags: `/O2 /arch:AVX2` baseline for app code; AVX-512 kernels compiled per-function via
  `__attribute__((target("avx512f,avx512vl,...")))` and selected by runtime CPUID, so the binary still
  runs on non-AVX512 CPUs.
- `/EHs-c- -D_HAS_EXCEPTIONS=0 /GR-` (no exceptions, no RTTI). Static CRT (`/MT`) for a single drop-in exe.
- Fallback if Ninja is unwanted: install VS "C++ Clang tools for Windows" component and use
  `-G "Visual Studio 18 2026" -T ClangCL`.

## 4. UI library decision

Requirements: tiny, native, fast to iterate, collapsible tree + checkboxes, progress bars, live
graphs, tables. Must not perturb benchmarks much.

| Option | Verdict |
|---|---|
| **Dear ImGui** | **Chosen.** Immediate mode fits "poll shared state every frame" perfectly. `CollapsingHeader`/`TreeNode` + `Checkbox` for the suite tree, `ProgressBar`, `BeginTable` for results, `PlotLines` for sparklines. Win32 + D3D11 backends are mature. Plain C-ish API, no callbacks/OOP. ~10 source files to vendor. |
| ImPlot (ImGui addon) | **Optional add-on** for nicer live graphs (scrolling throughput plots). Add only if `PlotLines` isn't enough. |
| Nuklear | Single-header C, similar model, but smaller ecosystem, weaker tables/plots, more manual styling. |
| microui | Too minimal (no tables/plots, need own renderer). |
| RmlUi / Qt / wxWidgets / WinUI | Too heavy or retained-mode/OOP; against the lightweight, C-style goal. |
| Raw Win32 controls | Lightest binary but painful for live graphs/tables; slow to iterate. |

UI renderer: **ImGui Win32 + DirectX 11 backend.** D3D11 is the least code and is independent from the
D3D12/Vulkan devices used by GPU benchmarks.

Interference rules:
- While a benchmark runs, UI throttles to ~10–15 FPS (sleep / `WaitForSingleObject` on a timer),
  VSync on. While idle, render only on input/at low rate.
- UI runs on the main thread; benchmark runs on one worker thread (which may spawn its own pinned
  pool). Multi-thread CPU tests will share a core with the (mostly sleeping) UI — acceptable, note it.
- UI D3D11 device uses the default adapter. Revisit (e.g. pin UI to the iGPU) only if it measurably
  disturbs dGPU results.

Vendoring: copy a pinned Dear ImGui release (`master`, not docking) into `third_party/imgui/` (only core
+ `imgui_impl_win32` + `imgui_impl_dx11`). No network needed at build time. Record version in
`third_party/README.md`.

## 5. Architecture

Procedural, data-oriented, C-style C++ (see CLAUDE.md for the rules).

```
src/
  main.cpp            Win32 window, D3D11 device/swapchain, ImGui frame loop
  ui.cpp/.h           Suite tree, run controls, live panel, results table
  bench.h             Core types: BenchDesc, BenchLive, BenchResult, Metric, registry table
  runner.cpp/.h       Worker thread: walks selected tests, fills live/results, honors cancel
  sysinfo.cpp/.h      CPUID (vendor, model, features, cache sizes), RAM, adapters, drives, power state
  timer.h             QPC wrapper, rdtsc helpers, DoNotOptimize/ClobberMemory
  threads.cpp/.h      Minimal pinned thread pool (Win32 threads, SetThreadGroupAffinity, barriers)
  mem_util.h          Aligned/large-page alloc (VirtualAlloc), arena
  cpu/cpu_bench.cpp
  mem/mem_bench.cpp
  disk/disk_bench.cpp
  gpu/gpu_common.h    Shared test params/metrics for both APIs
  gpu/gpu_vk.cpp      Vulkan backend (device, buffers, pipelines, timestamps)
  gpu/gpu_d3d12.cpp   D3D12 backend
  gpu/shaders/*.hlsl  Single HLSL source → DXIL (D3D12) and SPIR-V (Vulkan) via dxc at build time
third_party/imgui/
docs/PLAN.md
```

### Core data model (sketch)

```cpp
enum Suite : u8 { SUITE_CPU, SUITE_MEM, SUITE_GPU_VK, SUITE_GPU_D3D12, SUITE_DISK, SUITE_COUNT };

struct Metric      { const char* name; const char* unit; double value; };   // e.g. "Read", "GB/s"

struct BenchLive {                       // written by worker, read by UI (no locks)
    std::atomic<float>    progress;      // 0..1 for current test
    std::atomic<double>   current;       // latest instantaneous value (same unit as primary metric)
    std::atomic<u32>      phase;         // index into a per-test phase name table
    float                 history[256];  // ring buffer of samples for the live graph
    std::atomic<u32>      history_head;
};

struct BenchResult { u32 metric_count; Metric metrics[8]; u8 status; /* ok/skipped/failed/cancelled */ };

typedef void (*BenchFn)(const struct BenchParams* p, BenchLive* live, BenchResult* out,
                        const std::atomic<bool>* cancel);

struct BenchDesc { Suite suite; const char* group; const char* name; BenchFn fn; u32 flags; };

extern const BenchDesc g_benches[];      // one static table = the whole registry
extern const u32       g_bench_count;
// parallel arrays indexed by bench id: bool g_selected[]; BenchResult g_results[]; BenchLive g_live (only one runs at a time)
```

- No virtuals, no inheritance: a benchmark is a function pointer + a row in a table.
- Selection state, results, timings are flat arrays indexed by bench id.
- Live stats: lock-free single-writer/single-reader; torn reads of history are harmless.
- Each test runs **time-boxed** (e.g. ~2–4 s per measured phase, configurable) with a warm-up, then
  reports best (and optionally median) of N passes.

### Runner flow

1. UI builds a run list from `g_selected`.
2. Worker thread: for each test → reset `BenchLive` → call `fn` → store `BenchResult` → next.
3. UI each frame: draws tree with per-test status icon (pending/running/done/failed), overall
   progress (tests done / total + current test progress), current value + sparkline, elapsed time.
4. Cancel button sets atomic flag; every bench checks it between iterations.
5. End: results table grouped by suite, plus "Copy as text" and "Save CSV/JSON" (with sysinfo header).

## 6. Benchmark catalog

Units are raw throughput. Multi-thread (MT) tests use all logical cores unless noted; also show ST
(1 thread, pinned to the fastest core per `GetSystemCpuSetInformation` efficiency class/ordering) where meaningful.

### 6.1 CPU

| Test | What | Metric |
|---|---|---|
| Integer ALU | Dependent-chain-free int add/mul/xor mix, ST + MT | Gops/s |
| FP32 FMA scalar | Independent FMA chains (enough accumulators to hide latency) | GFLOPS |
| FP32 FMA AVX2 | 256-bit FMA, ≥8 accumulators | GFLOPS |
| FP32/FP64 FMA AVX-512 | 512-bit FMA (runtime-gated by CPUID) | GFLOPS |
| SGEMM | Cache-blocked single-precision matmul (e.g. 1024–2048²), MT | GFLOPS |
| Hashing | SHA-256 (use SHA-NI if available, else scalar) and/or xxHash-style 64-bit hash | GB/s |
| AES | AES-NI / VAES CTR-mode encrypt | GB/s |
| Sort | Sort 16M u32 (std::sort ST; parallel merge sort MT) — branchy/memory-mixed | Melem/s |
| Prime sieve / branchy | Segmented sieve, branch-heavy integer workload | Mnum/s |
| Core-to-core latency | Cache-line ping-pong between pinned thread pairs (matrix or min/avg) | ns |
| Thread scaling (optional) | FMA test at 1,2,4,8,16 threads | GFLOPS per count |

Notes: pin threads via `SetThreadGroupAffinity`; prefer one thread per physical core first for
"physical" MT runs, then SMT. Keep results from being optimized away (`DoNotOptimize`).

### 6.2 Memory

| Test | What | Metric |
|---|---|---|
| Read BW | AVX2 loads over buffer ≫ L3 (e.g. 1 GiB), MT | GB/s |
| Write BW | Normal stores and non-temporal (`_mm256_stream_si256`) stores | GB/s |
| Copy BW | memcpy-style + NT copy | GB/s |
| Triad (STREAM) | a = b + s*c | GB/s |
| Latency | Random pointer chase (shuffled cycle, one access per cache line, defeat prefetcher) at sizes 16 KiB … 1 GiB | ns per access |
| Cache BW sweep | Read BW per working-set size (L1/L2/L3/DRAM plateaus), ST + MT | GB/s per size |

Notes: allocate with `VirtualAlloc`; optionally large pages (needs SeLockMemoryPrivilege — try,
fall back silently). Touch all pages before timing. For latency use 4 KiB-page TLB effects as-is but
consider large pages to isolate DRAM latency.

### 6.3 GPU (Vulkan **and** D3D12, same tests, same HLSL)

Adapter picker (enumerate both GPUs; default discrete). All timing via **GPU timestamp queries**,
wall clock only as sanity check. Rendering is **offscreen** (render targets, no present) at a fixed
resolution (default 1920×1080, configurable) so results don't depend on the UI window or vsync.

Compute:
| Test | Metric |
|---|---|
| FP32 FMA throughput (`mad` chains, many waves) | TFLOPS |
| FP16 throughput (packed half, if supported) | TFLOPS |
| INT32 throughput | TIOPS |
| VRAM bandwidth (buffer read / write / copy, large buffers) | GB/s |
| Groupshared (LDS) bandwidth | GB/s |
| Host↔device transfer (upload & readback through staging/copy queue) | GB/s |

3D:
| Test | Metric |
|---|---|
| Triangle/vertex throughput (dense instanced mesh, tiny tris, trivial PS) | Mtri/s |
| Pixel fill rate (full-screen quads, overdraw, RGBA8, no blend + blend) | Gpix/s |
| Texture fill rate (bilinear sampling, multiple textures) | Gtex/s |
| Draw-call throughput (many small draws, CPU submission bound) | Kdraws/s |
| Scene test (instanced lit geometry + depth, several passes) | FPS / ms per frame |

2D:
| Test | Metric |
|---|---|
| Sprite batch (alpha-blended textured quads, many layers) | Msprites/s |
| Texture copy / blit (RT→RT, `CopyResource` / `vkCmdCopyImage` / `vkCmdBlitImage`) | GB/s |
| Clear rate (render-target clears) | GB/s |
| (Stretch) Direct2D path: geometry fill + text, D3D side only | ops/s |

Shader pipeline: author HLSL once, compile with `dxc -T cs_6_x/vs_6_x/ps_6_x` → DXIL, and
`dxc -spirv -fspv-target-env=vulkan1.3` → SPIR-V, at build time via CMake custom commands; embed as
byte arrays in generated headers. Use the Vulkan SDK's dxc (has SPIR-V codegen).

Live stats for GPU: per-batch timestamp readback → current rate into `BenchLive`.

### 6.4 Disk (CrystalDiskMark-style)

Default profile mirrors CDM 8 "Default":
| Test | Block | Queue depth | Threads |
|---|---|---|---|
| SEQ1M Q8T1 read / write | 1 MiB | 8 | 1 |
| SEQ1M Q1T1 read / write | 1 MiB | 1 | 1 |
| RND4K Q32T1 read / write | 4 KiB | 32 | 1 |
| RND4K Q1T1 read / write | 4 KiB | 1 | 1 |
Optional extra: RND4K Q32T16 (CDM "NVMe" profile), mixed 70/30.

Metrics: MB/s (decimal, like CDM), IOPS, average latency (µs). Run 5 passes, report best (CDM default),
with ~5 s per pass and a short interval between.

Implementation:
- Target directory selectable (default `C:\`, warn if target is a VHD/Dev Drive like K:). Test file
  `ezbench.tmp`, default 1 GiB (options 64 MiB … 8 GiB), deleted at end (and on cancel/crash at next start).
- `CreateFileW` with `FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED` (+ `FILE_FLAG_WRITE_THROUGH` for writes)
  to bypass the OS cache. Buffers from `VirtualAlloc` (page-aligned ⇒ sector-aligned). Offsets/sizes
  multiples of the physical sector size (`IOCTL_STORAGE_QUERY_PROPERTY` → `StorageAccessAlignmentProperty`).
- Queue depth via N outstanding `ReadFile`/`WriteFile` OVERLAPPED requests per thread, completion via
  IOCP (`GetQueuedCompletionStatusEx`), resubmit on completion.
- Prepare file by writing it fully with **random (incompressible) data** before read tests.
  `SetFileValidData` would skip that but needs SeManageVolumePrivilege — not worth it.
- Random offsets: fast PRNG (xorshift/pcg) aligned to block size across the whole file.
- Check free space before creating the file.

## 7. Live stats & UX

- Left: suite tree (`CollapsingHeader` per suite → `TreeNode` per group → checkbox per test,
  with select-all per suite). Status glyph + final primary value inline after each test.
- Right/top: current test name, phase, progress bar, current value with unit, scrolling sparkline,
  elapsed/ETA, overall progress bar.
- Bottom/end: results table (Suite | Test | Metric | Value | Unit), copy/export.
- Settings panel: GPU adapter, GPU resolution, disk target + file size + passes, test duration,
  thread count override.
- System info header: CPU, cores, caches, RAM, GPUs + drivers, OS, power source / power mode.

## 8. Milestones

- **M0 Skeleton**: CMake + presets, vendored ImGui, Win32+D3D11 window, bench registry, runner thread,
  one dummy bench showing live progress, cancel, results table. sysinfo basics.
- **M1 CPU** suite.
- **M2 Memory** suite.
- **M3 Disk** suite.
- **M4 GPU infra**: shader build step (dxc → DXIL + SPIR-V), Vulkan + D3D12 device init on chosen
  adapter, timestamp queries, compute tests.
- **M5 GPU 3D + 2D** tests (both APIs).
- **M6 Polish**: export CSV/JSON, copy-to-clipboard, settings persistence (simple ini), run-to-run
  compare (load a previous JSON and show deltas).

Each milestone: builds clean (no warnings at `-W4`-ish level), runs on this machine, numbers sanity-checked
against known values (see §10).

## 9. Benchmarking pitfalls (knowledge)

- **Power**: on battery or "Best power efficiency" mode, numbers drop a lot. Read
  `GetSystemPowerStatus` and show a warning; show active power mode if obtainable.
- **Thermals/boost**: laptop boost decays over seconds. Warm-up pass, fixed duration, report best +
  median; keep test order stable so runs are comparable.
- **Timers**: `QueryPerformanceCounter` for wall time (10 MHz on modern Windows). `rdtsc` is invariant
  on Zen 4 but not core-clock; use only for short relative timing.
- **Dead code elimination**: every result must feed a sink (`DoNotOptimize` via empty inline asm with
  a register/memory operand — clang-cl supports GNU `__asm__`).
- **Thread pinning**: Windows has processor groups; with 16 LPs we're in one group but use the
  group-aware API anyway. Zen 4 SMT siblings are adjacent LP indices (0/1, 2/3, ...) — verify via
  `GetLogicalProcessorInformationEx`.
- **Priority**: raise process to `HIGH_PRIORITY_CLASS` during runs (not REALTIME).
- **Memory**: Windows zeroes pages on first touch — pre-fault buffers before timing.
- **Disk**: NVMe SLC cache inflates sequential writes on small files; 1 GiB default is CDM-like. File
  system cache must be bypassed (NO_BUFFERING). Defender real-time scan can interfere on file
  create — single file, reused across tests, minimizes this.
- **GPU**: first dispatch includes pipeline/driver warm-up — always warm up. Use timestamp queries
  (`ID3D12CommandQueue::GetTimestampFrequency`, Vulkan `timestampPeriod`). Lock nothing — we can't set
  stable clocks on consumer drivers, so use longer runs. Hybrid laptops: make sure the D3D12/Vulkan
  device is created on the chosen adapter (`IDXGIFactory6::EnumAdapterByGpuPreference`, match Vulkan
  physical device by LUID via `VkPhysicalDeviceIDProperties`).
- **NVIDIA Optimus**: exporting `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` makes the
  *whole process* prefer the dGPU — don't, since we choose adapters explicitly and the UI should stay light.

## 10. Sanity reference values (rough expectations, fill with measured numbers)

| Metric | Expected ballpark |
|---|---|
| DRAM read BW (MT) | TBD — depends on LPDDR5X vs DDR5 config; check with `wmic memorychip` / sysinfo |
| DRAM latency | ~90–130 ns typical for mobile Zen 4 |
| Samsung PM9C1a-class NVMe SEQ1M Q8T1 read | multiple GB/s (PCIe 4.0 x4) |
| RTX 5060 Laptop FP32 | several TFLOPS (depends on TGP/clock) |

## 11. Open questions (defaults assumed until answered)

1. DirectX version for GPU tests: **D3D12** (assumed; parity with Vulkan). Also D3D11? (not planned)
2. GPU tests offscreen only (assumed), or also show a preview window while they run?
3. Disk default target `C:\`, 1 GiB, 5 passes (assumed).
4. Run GPU suite on both GPUs in one go, or one selected adapter (assumed: one selected)?
5. Results persistence / comparison history wanted early, or M6 is fine (assumed: M6)?

## 12. Decision log

- 2026-11-11: UI = Dear ImGui (Win32 + D3D11 backend), vendored. Build = CMake + Ninja + clang-cl.
  GPU benches = Vulkan + D3D12 from shared HLSL via dxc. C-style, data-oriented code; no exceptions/RTTI.
