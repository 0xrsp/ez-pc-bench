#pragma once
// Shared contract for the GPU suites. Both backends (gpu_d3d12.cpp, gpu_vk.cpp) run the SAME tests with
// the SAME shaders (src/gpu/shaders/*.hlsl -> DXIL + SPIR-V at build time) and the SAME workload
// constants below, so D3D12 and Vulkan numbers are directly comparable.
//
// ---- Shader binding rules (one HLSL source, two APIs) -------------------------------------------
//   b0  : push constants (Vulkan) / 8 root constants (D3D12): `struct Params { uint4 u; float4 f; }` (32 B)
//   uN  : UAVs            -> Vulkan binding N        (storage buffer / storage image)
//   tN  : SRVs            -> Vulkan binding 8 + N    (sampled image / storage buffer read-only)
//   sN  : samplers        -> Vulkan binding 16 + N   (s0 = linear filter, wrap addressing, everywhere)
//   All in set/space 0. The SPIR-V build uses -fvk-t-shift 8 / -fvk-s-shift 16 to implement this.
//   Vulkan: flip Y with a negative viewport height (core 1.1) so images match D3D12 orientation.
//
// ---- Timing rules ------------------------------------------------------------------------------
//   Compute tests: GPU timestamps around each dispatch (or dispatch batch); report via gpu_report_compute.
//   Transfer test: wall clock (QPC) from submit to fence completion.
//   3D / 2D tests: render OFFSCREEN (no swapchain) to an RGBA8_UNORM target of BenchParams::gpu_width x
//   gpu_height. At most GPU_FRAMES_IN_FLIGHT frames in flight; frame time = QPC delta between consecutive
//   frame submissions (after waiting for frame N - GPU_FRAMES_IN_FLIGHT), i.e. present-to-present without
//   a swapchain. Warm-up gpu_warmup_seconds() (discarded), then BenchParams::duration_ms of frames.
//   live_sample() the current FPS about every 50 ms, live_progress() over warm-up + measurement.
//
// ---- Preview -------------------------------------------------------------------------------------
//   During 3D / 2D tests, every >= GPU_PREVIEW_INTERVAL seconds: run cs_downscale (render target ->
//   preview texture of gpu_preview_size()), copy it to a readback buffer, and once that frame's fence
//   has completed call gpu_preview_publish(). Call gpu_preview_clear() when the test ends.
#include "core/bench.h"
#include "core/sysinfo.h"

typedef std::atomic<bool> CancelFlag;

// X(id, enum, group name, group cli id, test name, flags) — order = run order within each GPU suite.
#define GPU_TESTS(X)                                                                                    \
    X(fp32,      GT_FP32,         "Compute", "compute", "FP32 throughput",             0)               \
    X(fp16,      GT_FP16,         "Compute", "compute", "FP16 throughput (packed)",    0)               \
    X(int32,     GT_INT32,        "Compute", "compute", "INT32 throughput",            0)               \
    X(fp64,      GT_FP64,         "Compute", "compute", "FP64 throughput",             0)               \
    X(dp4a,      GT_DP4A,         "Compute", "compute", "INT8 dot product (dp4a)",     0)               \
    X(matrix,    GT_MATRIX,       "Compute", "compute", "Matrix multiply (FP16 / INT8)", 0)             \
    X(atomics,   GT_ATOMICS,      "Compute", "compute", "Atomics",                     0)               \
    X(vram,      GT_VRAM_BW,      "Compute", "compute", "VRAM bandwidth",              0)               \
    X(lds,       GT_LDS_BW,       "Compute", "compute", "Shared memory (LDS) bandwidth", 0)             \
    X(transfer,  GT_TRANSFER,     "Compute", "compute", "Host <-> GPU transfer",       0)               \
    X(latency,   GT_MEM_LATENCY,  "Memory",  "mem",     "Memory latency (pointer chase)", 0)            \
    X(sweep,     GT_MEM_SWEEP,    "Memory",  "mem",     "Read bandwidth sweep",        0)               \
    X(pso,       GT_PSO,          "Pipelines", "pso",   "Pipeline creation (cold / cached)", 0)         \
    X(formats,   GT_TEX_FORMATS,  "Texture", "tex",     "Sampling rate per format",    0)               \
    X(aniso,     GT_TEX_ANISO,    "Texture", "tex",     "Anisotropic filtering",       0)               \
    X(triangles, GT_TRIANGLES,    "3D",      "3d",      "Triangle throughput",         BENCH_PREVIEW)   \
    X(fill,      GT_PIXEL_FILL,   "3D",      "3d",      "Pixel fill rate",             BENCH_PREVIEW)   \
    X(texfill,   GT_TEXTURE_FILL, "3D",      "3d",      "Texture fill rate",           BENCH_PREVIEW)   \
    X(drawcalls, GT_DRAW_CALLS,   "3D",      "3d",      "Draw calls",                  BENCH_PREVIEW)   \
    X(scene,     GT_SCENE,        "3D",      "3d",      "Lit scene",                   BENCH_PREVIEW)   \
    X(sprites,   GT_SPRITES,      "2D",      "2d",      "Sprite batch",                BENCH_PREVIEW)   \
    X(blit,      GT_BLIT,         "2D",      "2d",      "Blit / copy",                 BENCH_PREVIEW)   \
    X(clear,     GT_CLEAR,        "2D",      "2d",      "Clear",                       BENCH_PREVIEW)

enum GpuTestId : u8 {
#define X(id, e, g, gid, name, flags) e,
    GPU_TESTS(X)
#undef X
    GT_COUNT
};

// ---- Workloads (identical for both APIs) -----------------------------------------------------------

// ALU (alu.hlsl): GC_ALU_GROUPS x 256 threads, pc.u.x = GC_ALU_ITERS loop iterations, 64 ops per
// thread per iteration (8 x 4-wide multiply-add, counted as 2 ops each).
constexpr u32 GC_ALU_GROUPS = 4096;
constexpr u32 GC_ALU_ITERS = 1024;
constexpr f64 GC_ALU_OPS_PER_DISPATCH = 64.0 * GC_ALU_ITERS * GC_ALU_GROUPS * 256;

// VRAM bandwidth (bandwidth.hlsl): two GC_VRAM_BYTES device-local buffers. GC_BW_GROUPS x 256 threads,
// each walks the buffer with a 16-byte stride of (GC_BW_GROUPS * 256). pc.u.x = number of 16-byte
// elements, pc.u.y = thread count. Bytes moved: read = N, write = N, copy = 2N.
constexpr u64 GC_VRAM_BYTES = 256 * MiB;
constexpr u32 GC_BW_GROUPS = 2048;

// LDS (lds.hlsl): GC_LDS_GROUPS x 256 threads, pc.u.x = GC_LDS_ITERS, 64 bytes read from groupshared
// memory per thread per iteration.
constexpr u32 GC_LDS_GROUPS = 4096;
constexpr u32 GC_LDS_ITERS = 512;
constexpr f64 GC_LDS_BYTES_PER_DISPATCH = 64.0 * GC_LDS_ITERS * GC_LDS_GROUPS * 256;

// Host <-> GPU: GC_TRANSFER_BYTES per copy, upload (host-visible -> device-local) and readback.
constexpr u64 GC_TRANSFER_BYTES = 128 * MiB;

// 3D / 2D frames (raster.hlsl). Resolution = BenchParams::gpu_width x gpu_height.
constexpr u32 GPU_FRAMES_IN_FLIGHT = 2;
constexpr u32 GR_TRI_COUNT = 2000000;       // vs_triangles: tiny triangles per frame, non-indexed, 3 verts each
constexpr u32 GR_FILL_LAYERS = 16;          // vs_fullscreen + ps_fill: full-screen layers (instances), opaque
constexpr u32 GR_TEX_LAYERS = 4;            // vs_fullscreen + ps_texfill: layers, each samples 4 textures
constexpr u32 GR_TEX_SIZE = 1024;           // 4 RGBA8 noise textures GR_TEX_SIZE^2 (t0..t3), 1 mip
constexpr u32 GR_DRAWS = 10000;             // vs_drawcall + ps_color: separate 1-triangle draws per frame
constexpr u32 GR_SCENE_CUBES = 20000;       // vs_scene + ps_scene: instanced cubes (36 verts), depth D32
constexpr u32 GR_SPRITES = 100000;          // vs_sprite + ps_sprite: alpha-blended quads (6 verts), t0 = sprite
constexpr u32 GR_SPRITE_TEX = 64;           // sprite texture size (RGBA8, procedural, alpha disc)
constexpr u32 GR_BLIT_COPIES = 8;           // full render-target copies per frame (RT -> second RT)
constexpr u32 GR_CLEARS = 32;               // full render-target clears per frame (color changes per clear)

// ---- Preview hand-off (backend -> UI) -------------------------------------------------------------

constexpr u32 GPU_PREVIEW_MAX_W = 640, GPU_PREVIEW_MAX_H = 400;
constexpr f64 GPU_PREVIEW_INTERVAL = 1.0 / 15.0;

void gpu_preview_size(u32 rt_w, u32 rt_h, u32* w, u32* h);         // fits MAX, keeps aspect, w multiple of 64
void gpu_preview_publish(const u8* rgba, u32 w, u32 h, u32 row_pitch);
void gpu_preview_clear();
// UI side: copies the latest frame if newer than *seq. dst holds GPU_PREVIEW_MAX_W * MAX_H * 4 bytes
// (tightly packed, w * 4 per row). Returns false when nothing new; *w = *h = 0 when no preview is active.
bool gpu_preview_acquire(u32* seq, u8* dst, u32* w, u32* h);

// ---- Measurement helpers ---------------------------------------------------------------------------

const GpuInfo* gpu_selected(const BenchParams* p);    // adapter chosen in settings (match by LUID)
f64 gpu_warmup_seconds(const BenchParams* p);         // min(1 s, duration / 3)

// Frame log for 3D / 2D tests.
void gpu_frames_begin();
void gpu_frames_push(f32 frame_ms);
// Adds: Average FPS, 1% low, 0.1% low (fps), Avg / p99 / p99.9 frame time (ms) and, if work_name != null,
// the derived throughput work_per_frame * frames / seconds / unit_div in work_unit.
void gpu_frames_report(BenchResult* out, f64 work_per_frame, const char* work_name, const char* work_unit, f64 unit_div);

// Compute tests: adds "Throughput" (total work / total time), "Throughput (fastest dispatch)",
// and "Dispatch time min/avg/max" (ms).
void gpu_report_compute(BenchResult* out, const char* unit, f64 unit_div, f64 work_per_dispatch,
                        const f64* dispatch_ms, u32 n);
// Average rate of n timed dispatches doing `work` each (work units per second).
f64 gpu_rate(f64 work, const f64* dispatch_ms, u32 n);

// ---- Suites (gpu_multi.cpp) --------------------------------------------------------------------------
// The registry's GPU suites: one backend device per adapter in gpu_adapter_mask(); every test runs on each
// adapter in turn. With several adapters, metric names get the adapter's short name as prefix
// ("rtx5060-laptop: Throughput"), like the disk suite's "C: Read"; with one they pass through unchanged.
u32 gpu_adapter_mask(const BenchParams* p);           // BenchParams::gpu_adapters, or just gpu_adapter
bool gpu_vk_suite_init(const BenchParams* p, void** state, char* error, u32 error_size);
bool gpu_d3d12_suite_init(const BenchParams* p, void** state, char* error, u32 error_size);
void gpu_suite_shutdown(void* state);
void gpu_suite_run(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                   const CancelFlag* cancel);

// ---- Backends (one implementation file each) -------------------------------------------------------

bool gpu_d3d12_init(const BenchParams* p, void** state, char* error, u32 error_size);
void gpu_d3d12_shutdown(void* state);
void gpu_d3d12_run(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                   const CancelFlag* cancel);

bool gpu_vk_init(const BenchParams* p, void** state, char* error, u32 error_size);
void gpu_vk_shutdown(void* state);
void gpu_vk_run(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                const CancelFlag* cancel);
