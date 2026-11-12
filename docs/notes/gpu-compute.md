# GPU compute / memory / pipelines / multi-adapter (FUTURE.md Â§5)

Stream notes, 2026-11-11. What was built, how it works, measured numbers, and what's left. For PLAN.md / FUTURE.md
to absorb when merged.

## FUTURE.md Â§5 items

| Item | Status |
|---|---|
| P2 GPU memory hierarchy (pointer-chase latency + bandwidth sweep, series) | done, both APIs |
| P2 FP64 | done, both APIs |
| P2 INT8 dot4 (dp4a) | done, both APIs |
| P2 FP16 / INT8 matrix via `VK_KHR_cooperative_matrix` | done on Vulkan; **D3D12 skipped with a reason** (below) |
| P2 atomics throughput | done, both APIs |
| P2 pipeline / shader compile time, cold vs cached | done, both APIs |
| P2 several adapters in one run | done (suite layer, CLI, GUI, ini, TOON, compare) |
| P3 texture format sampling rates (BC1â€“BC7, R32F, ...) | done, both APIs |
| P3 anisotropic filtering cost | done, both APIs |

D3D12 matrix: the released runtime / DXC have no matrix-multiply HLSL. `D3D12_OPTIONS9.WaveMMATier` exists but the
WaveMMA intrinsics never shipped in a retail compiler; SM 6.9 cooperative vectors need the preview Agility SDK
(D3D12Core.dll shipped next to the exe, a new dependency). The test reports skipped: "no matrix-multiply HLSL in
the released D3D12 runtime / DXC (driver WaveMMA tier N); Vulkan only".

## Tests (ids, metrics)

New `GPU_TESTS` rows (gpu_common.h), same ids under `gpu_vk.` and `gpu_d3d12.`; all default-on.

| Id | Metrics | Notes |
|---|---|---|
| `compute.fp64` | Throughput, fastest, dispatch min/avg/max (TFLOPS, ms) | `cs_fp64`, 64 iterations (1/16 of FP32). Skip: Vulkan `shaderFloat64`, D3D12 `DoublePrecisionFloatShaderOps` |
| `compute.dp4a` | same, TOPS | `dot4add_i8packed` (SM 6.4; SPIR-V `OpSDot`, needs `shaderIntegerDotProduct`). 8 ops per dot4add |
| `compute.matrix` | `FP16 (FP32 accumulate)` TFLOPS, `INT8 (INT32 accumulate)` TOPS | Vulkan only, GLSL `coopmat.comp` (glslc); shape from `vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` (16x16x16 preferred), one subgroup per workgroup (required subgroup size) |
| `compute.atomics` | `One address`, `Unique addresses`, `Shared memory (LDS)` (Gatomics/s) | `InterlockedAdd` without return value; each variant one phase of `duration_ms` |
| `mem.latency` | series `Latency` (ns) vs working set 4 KiB .. 512 MiB (256 MiB if < 4 GiB VRAM) | single-thread chase, 128-byte stride, random cycle (Sattolo), each dispatch continues where the last stopped |
| `mem.sweep` | series `Read` (GB/s) vs working set 16 KiB .. 512 MiB / 256 MiB | 1024 groups x 256 threads x 512 uint4 loads, wrapping inside the working set |
| `pso.pso` | `Compute, cold` / `Compute, cached` / `Graphics, cold` / `Graphics, cached` (PSO/s), `Compute, cold, slowest` / `Graphics, cold, slowest` (ms) | see below |
| `tex.formats` | one metric per format: RGBA8, R32F, RGBA16F, RGBA32F, BC1, BC3, BC4, BC5, BC6H, BC7 (Gtex/s) | 2048Â² random texels / blocks, bilinear, 2048x1024 threads x 128 samples; formats the device can't sample/filter are left out |
| `tex.aniso` | Bilinear, Trilinear, Anisotropic 2x / 4x / 8x / 16x (Gtex/s) | RGBA8 2048Â² + full mips, `SampleGrad` with a 16:1 footprint |

Timing: GPU timestamps around every dispatch (as the base compute tests). Series points take max(0.1 s, duration/16);
formats max(0.2 s, duration/8) each; aniso modes max(0.2 s, duration/6) each. PSO: each cold pass stops after 40 % of
the duration or 128 pipelines; the cached pass recreates the same set.

### Pipeline creation, cold vs cached

- Shaders (`pso.hlsl`): a game-like PBR pixel shader (normal map, 8 GGX lights, 3x3 PCF, fog, ACES), a skinned
  procedural vertex shader, a 6-octave noise compute shader with an LDS blur. Graphics PSO: RGBA8 + D32, depth test,
  alpha blend, back-face culling.
- Cold must miss every driver cache (in-process and on-disk), and both persist across runs, so every pipeline gets a
  new random `SEED`:
  - Vulkan: specialization constant 0.
  - D3D12: no specialization constants. `SEED` is a literal (0x5EED1234) used only as an xor operand, so DXIL holds it
    as one integer-constant record: VBR8 of `v << 1`. `gpu_d3d12_pso.cpp` finds that 40-bit pattern once (exactly one
    hit per shader, checked) and writes a new value of the same encoded length into a copy of the container, then
    recomputes the container checksum (DXBC modified-MD5; verified against DXC's own digest at test start).
    **Only changing the `HASH` part + checksum was not enough**: NVIDIA served those from its cache (~1.2 ms vs ~35 ms).
- Cached: cold pipelines go into an app cache (Vulkan `VkPipelineCache`, D3D12 `ID3D12PipelineLibrary` via
  `StorePipeline`), which is serialized, destroyed, recreated from the blob, and the same pipelines are created again
  from it (a game's pipeline cache file on the next start).

## Multi-adapter

- `BenchParams::gpu_adapters` (bitmask of `g_sys.gpus`, 0 = only `gpu_adapter`). `gpu_adapter` stays the primary
  (lowest selected): monitor sensors, the save-name default and `params.gpu` use it.
- `gpu_multi.cpp`: the registry's GPU suites are now `gpu_vk_suite_init` / `gpu_d3d12_suite_init` /
  `gpu_suite_shutdown`; `gpu_entry.cpp` wrappers call `gpu_suite_run`. Init creates one backend device per selected
  adapter (any failure fails the suite, prefixed with the adapter name). Each test runs on every adapter in turn;
  with one adapter results pass through unchanged (same metric names as before), with several each adapter's metrics
  are renamed `<short name>: <metric>` (`rtx5060-laptop: Throughput`, duplicates get `#<index>`). A failure on any
  adapter fails the test; a skip on every adapter skips it; a skip on some adapters just leaves their metrics out (the
  reason isn't kept).
- The Vulkan context is heap-allocated now (was a static `s_vk`), so two devices can coexist.
- CLI: `--gpu 1` unchanged; `--gpu 0,1` runs on both. GUI: the GPU combo is a checkbox list (at least one stays
  ticked), preview text `A + B`. ini: `gpu=` (primary) unchanged, plus `gpus=Name A;Name B` only when several are
  ticked. TOON: `params.gpus: A + B` only when several. Compare window GPU column shows `A + B`. The GUI chart draws
  one plot per run of same-named series points (one curve per adapter).

## Measured (2026-11-11, AC, "Best performance", 1 s phases, release, not elevated)

RTX 5060 Laptop (adapter 0) / Radeon 780M (adapter 1), from one `--gpu 0,1` run.

| Test | RTX 5060 VK | RTX 5060 D3D12 | 780M VK | 780M D3D12 |
|---|---|---|---|---|
| FP64 | 0.237 TFLOPS | 0.236 | 0.123 | 0.093 |
| dp4a | 23.2 TOPS | 34.2 | 6.9 | 11.6 |
| Coop matrix FP16 / INT8 | 35.9 TFLOPS / 133 TOPS | skipped | 11.5â€“15.6 / 10.1â€“15.1 | skipped |
| Atomics one / unique / LDS (G/s) | 50â€“76 / 203 / 367 | 78 / 135 / 353 | 115 / 140 / 449 | 165 / 144 / 455 |
| Latency L1 / L2 / VRAM | 18.6 / 118â€“155 / 331â€“341 ns | 17.5 / 117 / 342 ns | 13â€“17 (L0) / 41â€“56 / 116â€“200 / 250â€“300 ns | 12â€“13 / 34â€“39 / 67â€“76 / 203â€“289 ns |
| Sweep L1 / L2 / VRAM (GB/s) | 2284 / 1600 / 348â€“365 | 2285 / 1650â€“1830 / 286â€“331 | 2300 / 1100â€“1800 / 78â€“86 | 3250 / 1060â€“2420 / 83â€“91 |
| PSO compute cold / cached (PSO/s) | 23.6 / 14584 | 27.7 / 2335 | 26.7 / 57262 | 46.7 / 21564 |
| PSO graphics cold / cached (PSO/s) | 16.8 / 7872 | 28.6 / 2257 | 20.8 / 33911 | 17.1 / 9170 |
| Slowest cold PSO compute / graphics | 43 / 62 ms | 39 / 45 ms | 40 / 53 ms | 23 / 62 ms |
| Texture RGBA8 / R32F / RGBA16F / RGBA32F | 239 / 218 / 125 / 65 Gtex/s | 240 / 214 / 122 / 61 | 75 / 76 / 62 / 32 | 77 / 75 / 60 / 30 |
| Texture BC1 / BC3 / BC4 / BC5 / BC6H / BC7 | 236 / 185 / 226 / 239 / 124 / 192 | 232 / 187 / 224 / 237 / 122 / 192 | 78 / 78 / 80 / 78 / 77 / 77 | 80 / 78 / 79 / 78 / 77 / 78 |
| Aniso bilinear / tri / 2x / 4x / 8x / 16x | 142 / 133 / 108 / 69 / 36 / 16 | 141 / 134 / 105 / 69 / 36 / 16 | 79 / 76 / 43 / 22 / 11 / 5.6 | 75 / 81 / 43 / 22 / 11 / 5.6 |

Reading them:
- RTX 5060: FP64 = 1/64 of FP32; L1 ~18 ns up to 64 KiB, L2 ~117 ns up to 32 MiB (= its 32 MiB L2), VRAM ~340 ns.
  The sweep drops at 64 MiB the same way. Half-rate RGBA16F / BC6H and quarter-rate RGBA32F filtering; BC3 / BC7
  ~20 % slower than BC1. Aniso cost scales with the probe count above 2x (16x = 1/9 of bilinear).
- 780M (RDNA3 iGPU): steps after 16 KiB (L0), ~256 KiB (L1) and 2 MiB (L2), then system DDR5 (~200â€“300 ns, ~80 GB/s = the shared
  DDR5-5600 bus). Every format filters at full rate (~77 Gtex/s) except RGBA16F (0.8x) and RGBA32F (0.4x). The 780M
  latency curve on Vulkan is noisier (clock ramping on a 1-thread load); its first point read 34 ns before the
  warm-up was added for the first point.
- dp4a: D3D12 is ~1.5x (NVIDIA) and ~1.7x (AMD) faster than the same HLSL through SPIR-V `OpSDot` + add. Same for
  FP64 on the 780M the other way round (VK 0.12 vs D3D12 0.09). Driver codegen, not measurement: both use the same
  dispatch / timestamp code.
- One-address atomics are clearly aggregated per wave by the hardware / driver (76 G/s into one word on NVIDIA,
  165 G/s on the 780M), so they measure that path, not raw L2 atomic latency. NVIDIA's value swung 50â€“78 between runs.
- Cold PSO creation is ~25â€“45 ms per pipeline on both GPUs and APIs: one new pipeline per frame is a visible hitch at
  60 fps. Cached recreation is 100â€“2000x faster. NVIDIA D3D12 pipeline libraries load ~6x slower than its Vulkan
  pipeline cache.

## Verification

- Release build warning-free; debug build warning-free.
- All new tests on both APIs on the RTX 5060 (adapter 0) and the 780M (adapter 1), both alone and in one
  `--gpu 0,1` run; GUI autorun of four of them (results table) and the compare window with the multi-adapter file.
- Vulkan validation layer (debug build, both adapters): first run found two real issues (coop-matrix shader needed
  `maintenance4` for `LocalSizeId`; enabling `vulkanMemoryModel` made dxc's Device-scope atomics invalid without
  `vulkanMemoryModelDeviceScope`), both fixed; second run of every Vulkan compute / memory / PSO / texture test plus
  `3d.scene` and `2d.blit` was clean. The D3D12 debug layer isn't installed on this PC (Graphics Tools), so D3D12 was
  not layer-validated.
- Bugs found by measuring: DXC/DXIL reads `[unroll(N)]` as "at most N iterations" (D3D12 took 8 of 128 texture
  samples, 14x too fast; SPIR-V treats it as a hint) â€” removed; the chase restarted at element 0 every dispatch, so
  big working sets looked like L2 â€” the position now carries over in the sink buffer.

## Not done / caveats

- GUI multi-select combo not seen in a screenshot (needs the ini changed or a dev flag; the GUI has no `--gpu`).
- Live progress restarts at 0 for each adapter in a multi-adapter test.
- Sweep L1 bandwidth (~2.3 TB/s on the RTX 5060) is well below its theoretical L1 rate; the LDS test reaches
  8.7 TB/s. The kernel (one uint4 load + index math per iteration) is issue-bound at that level. Fine for the shape
  of the curve; don't read the first points as peak L1 bandwidth.
- No Infinity Cache on either GPU here, so that level of the sweep is untested.
