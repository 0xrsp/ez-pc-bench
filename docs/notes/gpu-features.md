# GPU feature tests (FUTURE.md §5 "Feature tests")

3DMark-style feature tests, new GPU group **Features** in both GPU suites. Same HLSL, same procedural
scenes and the same reported metrics on Vulkan and D3D12; each renders offscreen and feeds the live preview
like the 3D / 2D tests. Unsupported features are skipped with the reason.

## Tests

| Id (`gpu_vk.` / `gpu_d3d12.` +) | What runs | Reported |
|---|---|---|
| `features.raytracing` | Inline ray queries from a compute shader (DXR 1.1 `RayQuery` / `VK_KHR_ray_query`). Scene: 256x256-cell wavy terrain (131k tris, one BLAS deformed by a compute shader and **refitted every frame**) + 2048 sphere instances (one 1024-tri BLAS) in a TLAS **rebuilt every frame**. Per pixel 1 primary ray, on a hit 1 shadow + 2 AO rays. Rays are counted on the GPU (one atomic per wave). | Rays (Grays/s over the whole frame), Rays (trace pass only), frame times, trace pass / BLAS refit / TLAS build (GPU ms per frame), BLAS full build (terrain, sphere), rays per pixel |
| `features.mesh_shader` | 4096 bumpy spheres x 128 meshlets (81 verts / 128 tris each, 67M tris per frame) around a turning camera. Pass 1: classic indexed instanced draw (vertex shader). Pass 2: task shader culls meshlets (frustum + normal cone), mesh shader emits the survivors. Same triangles, back-face culling + depth in both. | Both frame times, speedup %, both p99, vertex pipeline Mtri/s |
| `features.tessellation` | 48x48 quad patches, uniform integer factor 64 (18.9M tris per frame), domain shader displaces a terrain. | Frame times, tessellated Mtri/s |
| `features.vrs` | 2 full-screen layers of an ALU-heavy procedural terrain pixel shader. Pass 1x1, pass with per-draw 2x2 rate (tier 1 / pipeline rate), pass with a foveated rate image (tier 2 / attachment rate: ellipse 1x1 in the center, 2x2 ring, 4x4 corners). | Frame time per pass, speedup %, share of coarse pixels |
| `features.sampler_feedback` | D3D12 only. 16 full-screen layers of a perspective ground plane sampling a 2048^2 mip-mapped texture; pass 2 also writes a MinMip feedback map (64x64 mip regions), clears and decodes it every frame. | Both frame times, feedback overhead %, share of mip regions sampled (sanity check: fails if the map stays empty) |
| `features.async_compute` | Graphics = 512 depth-only full-screen layers (no pixel shader, depth ALWAYS + write: a shadow-map-style pass, shader ALUs idle); compute = `cs_fp32` (ALU bound). Pass 1: graphics, a full barrier, compute, all on the graphics queue; GPU timestamps around both parts rescale the compute loop count during warm-up until both take equally long. Pass 2: compute on a compute queue. | Both frame times, overlap gain % (100 % = perfect overlap), graphics / compute part (GPU ms) |

Two- and three-pass tests run warm-up + duration per pass (the later passes get a short warm-up), so they take
2-3x the duration. All join the default selection (`BENCH_PREVIEW`, not `BENCH_DEFAULT_OFF`).

## Files

- `src/gpu/gpu_features.h/.cpp`: workload constants, procedural scenes shared by both APIs (terrain / sphere
  meshes, TLAS instance descriptors, meshlet index buffer, VRS rate image, mip-mapped feedback texture), pass
  statistics.
- `src/gpu/gpu_d3d12_features.cpp`, `src/gpu/gpu_vk_features.cpp`: the tests, each with its own offscreen target,
  preview and frame loop (same timing rules as `gpu_d3d12_raster.cpp` / `gpu_vk_tests.cpp`); pipelines and
  resources are created per test and freed after it.
- Shaders: `rt.hlsl` (cs_rt_terrain, cs_rt_trace: SM 6.5 RayQuery), `mesh.hlsl` (vs/ps/as/ms_mesh: as/ms SM 6.5),
  `tess.hlsl` (vs/hs/ds/ps_tess), `vrs.hlsl` (ps_vrs), `sfb.hlsl` (ps_sfb_plain / ps_sfb_feedback, DXIL only
  through the new `ezb_shader_dxil()`); VRS, sampler feedback and async reuse `vs_fullscreen`, async also
  `cs_fp32`. SPIR-V needs no extra flags: Vulkan SDK dxc 1.9 with `-fspv-target-env=vulkan1.3` emits
  `SPV_KHR_ray_query` and `SPV_EXT_mesh_shader` (not the NV variant) by itself.

## Edits to shared files (small, additive)

- `gpu_common.h`: 6 rows at the end of `GPU_TESTS` (group "Features", cli group `features`).
- `gpu_d3d12.cpp` / `gpu_vk_tests.cpp`: one `case` list in the run dispatch -> `d3d_feature_test` / `vk_feature_test`.
- `gpu_vk.cpp` `create_device`: `vk_features_prepare()` (in `gpu_vk_features.cpp`) chains the optional features
  after the 1.3 struct and lists the extensions; a compute-only queue family gets a queue (`comp_family` /
  `comp_queue`, async test). Enabled only when supported: VK_KHR_acceleration_structure + VK_KHR_ray_query +
  VK_KHR_deferred_host_operations (+ `bufferDeviceAddress`), VK_EXT_mesh_shader (meshShader, taskShader),
  VK_KHR_fragment_shading_rate (pipeline + attachment rate), core `tessellationShader`.
- `gpu_vk_internal.h`: those flags in `VkCtx` + `VkFeatureChain`.
- D3D12 needs no device changes (FL 12_0 device; features checked per test via OPTIONS5/6/7 and SM 6.5,
  ID3D12Device2/5/8 and command list 1/4/5/6 by QueryInterface).
- `CMakeLists.txt`: 13 shader entries, `ezb_shader_dxil()`, 3 new sources.

## Measured (2026-11-11, AC, "Best performance", 2 s phases, 1920x1080)

| | RTX 5060 Laptop Vulkan | RTX 5060 Laptop D3D12 | Radeon 780M Vulkan | Radeon 780M D3D12 |
|---|---|---|---|---|
| Rays (whole frame / trace pass only) | 4.56 / 5.64 Grays/s | 4.58 / 5.62 | 0.80 / 0.89 | 0.93 / 1.06 |
| RT frame / trace pass | 1.45 / 1.18 ms | 1.45 / 1.18 | 8.33 / 7.46 | 7.10 / 6.24 |
| BLAS refit (131k tris) / TLAS build (2049 inst.) | 0.13 / 0.13 ms | 0.13 / 0.11 | 0.42 / 0.29 | 0.42 / 0.30 |
| BLAS full build terrain / sphere | 0.62 / 0.21 ms | 0.55 / 0.32 | 11.9 / 0.86 | 12.0 / 0.88 |
| Vertex pipeline / mesh shader frame | 4.19 / 1.71 ms (+146 %) | 4.20 / 1.25 (+236 %) | 15.0 / 5.30 (+183 %) | 14.4 / 5.41 (+166 %) |
| Tessellation (factor 64, 18.9M tris) | 2.87 ms, 6.6 Gtri/s | 1.06 ms, 17.8 Gtri/s | 4.01 ms, 4.7 Gtri/s | 4.02 ms, 4.7 Gtri/s |
| VRS 1x1 / 2x2 / rate image | 1.20 / 0.31 / 0.52 ms | 1.18 / 0.31 / 0.52 | 2.77 / 0.72 / 1.29 | 2.86 / 0.73 / 1.32 |
| VRS speedup 2x2 / rate image (72 % coarse px) | +293 / +132 % | +283 / +126 % | +285 / +115 % | +289 / +116 % |
| Sampler feedback off / on | skipped | 0.36 / 1.52 ms (+319 %) | skipped | 1.22 / 19.97 ms (+1531 %) |
| Async serial / async frame | 4.60 / 2.60 ms (+77 %) | 4.62 / 2.64 (+75 %) | 4.41 / 2.83 (+56 %) | 4.55 / 2.85 (+60 %) |

Raw data in the run's TOON files (not committed). Observations:
- Every feature is supported on both GPUs and both APIs (sampler feedback is D3D12-only by design). Vulkan
  validation (debug build, incl. synchronization validation via `VK_LAYER_ENABLES`) printed nothing on either
  GPU. The D3D12 debug layer still isn't installed, so D3D12 wasn't layer-validated.
- Tessellation on the RTX 5060 is 2.7x faster on D3D12 than on Vulkan with the same HLSL; the 780M is equal.
  Likely how DXC lowers the hull shader to SPIR-V (the patch-constant function runs inside the TCS invocations
  behind a barrier) meeting NVIDIA's Vulkan path. Not investigated further.
- Mesh shaders cull ~2/3 of the work here (objects all around a turning camera). The vertex pipeline submits
  67M triangles per frame; the hardware rejects off-screen ones after the vertex shader (16 Gtri/s "submitted").
- Sampler feedback writes are expensive: +319 % on the RTX 5060 and 16x slower on the 780M for 16 full-screen
  layers with one feedback write per pixel each (plus clear + decode per frame). Engines write feedback for
  a fraction of pixels; this test writes it everywhere, as a worst case.
- Async compute: the first versions overlapped nothing (gain -18 % .. +8 %) with a pixel-shaded graphics load
  (tiny triangles, then fill rate) next to the saturating ALU kernel: no free shader slots. With a depth-only
  graphics pass both GPUs overlap well. The graphics part times out to ~2.3 ms on both GPUs (constant-depth
  full-screen layers hit the compressed depth fast path).
- Terrain BLAS full builds on the 780M take ~12 ms (vs 0.6 ms on the RTX 5060); refits stay cheap (0.4 ms).

## FUTURE.md §5 "Feature tests" status

- Ray tracing (DXR 1.1 inline + Vulkan ray query, Grays/s, BLAS / TLAS build and refit): **done**.
- Mesh shaders vs classic vertex pipeline, same culled scene, both passes: **done**.
- Tessellation (hull / domain): **done**. Variable-rate shading: **done** (per-draw 2x2 and screen-space rate
  image). Sampler feedback (D3D12): **done** (MinMip, with per-frame clear + decode).
- Async compute overlap (graphics + compute queue concurrency gain in %): **done**.
- Not done: ray tracing pipelines (DXR 1.0 TraceRay / VK_KHR_ray_tracing_pipeline with shader tables), VRS
  per-primitive rate (SV_ShadingRate), sampler feedback MipRegionUsed maps / texture-space shading, D3D12
  debug-layer validation.
