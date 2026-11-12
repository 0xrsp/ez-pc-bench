#pragma once
// Extra GPU tests shared by both APIs (FUTURE.md §5): FP64, INT8 dot product, matrix multiply (cooperative
// matrix), atomics, memory hierarchy (latency + bandwidth sweep), pipeline creation, texture sampling.
// Implementations: gpu_vk_ext*.cpp / gpu_d3d12_ext*.cpp; routed from gpu_multi.cpp, so the base backends'
// dispatch stays untouched. Base contract (bindings, timing, GPU timestamps): gpu_common.h.
#include "gpu/gpu_common.h"

// ---- Compute (compute_ext.hlsl) ---------------------------------------------------------------------
// FP64: GC_ALU_GROUPS x 256 threads, pc.u.x = GE_FP64_ITERS, 64 flops per thread per iteration (as FP32).
constexpr u32 GE_FP64_ITERS = 64;
constexpr f64 GE_FP64_OPS_PER_DISPATCH = 64.0 * GE_FP64_ITERS * GC_ALU_GROUPS * 256;
// dp4a: GC_ALU_GROUPS x 256 threads, pc.u.x = GC_ALU_ITERS, 8 dot4add per iteration x 8 ops (4 mul + 4 add).
constexpr f64 GE_DP4A_OPS_PER_DISPATCH = 64.0 * GC_ALU_ITERS * GC_ALU_GROUPS * 256;
// Atomics: GE_ATOM_GROUPS x 256 threads, pc.u.x = adds per thread (per variant below).
constexpr u32 GE_ATOM_GROUPS = 1024;
constexpr u32 GE_ATOM_ITERS_ONE = 16, GE_ATOM_ITERS_UNIQUE = 64, GE_ATOM_ITERS_LDS = 256;
// Cooperative matrix (Vulkan, coopmat.comp): GE_MATRIX_GROUPS subgroups, pc.u.x = GE_MATRIX_ITERS,
// 4 accumulators x 2*M*N*K ops per iteration.
constexpr u32 GE_MATRIX_GROUPS = 4096, GE_MATRIX_ITERS = 256;

// ---- Memory hierarchy (memory.hlsl) -------------------------------------------------------------------
// Latency: a random cyclic chain (Sattolo shuffle on the CPU) with one element per GE_CHASE_STRIDE bytes
// (one cache line on current GPUs), followed by a single thread. Working sets 4 KiB .. gpu_ext_mem_cap().
constexpr u32 GE_CHASE_STRIDE = 128;
constexpr u64 GE_CHASE_MIN = 4 * KiB;
constexpr f64 GE_CHASE_TARGET_MS = 2.0;      // steps per dispatch are sized for about this long
// Bandwidth sweep: GE_SWEEP_GROUPS x 256 threads, GE_SWEEP_LOADS uint4 loads each, wrapping inside the
// working set (16 KiB .. gpu_ext_mem_cap()).
constexpr u64 GE_SWEEP_MIN = 16 * KiB;
constexpr u32 GE_SWEEP_GROUPS = 1024, GE_SWEEP_LOADS = 512;
constexpr f64 GE_SWEEP_BYTES_PER_DISPATCH = 16.0 * GE_SWEEP_LOADS * GE_SWEEP_GROUPS * 256;
constexpr u32 GE_MEM_MAX_POINTS = 24;
// Largest working set: 512 MiB with >= 4 GiB dedicated VRAM, else 256 MiB (iGPUs share system RAM).
u64 gpu_ext_mem_cap(const BenchParams* p);
// Seconds per series point: duration / 16, at least 0.1 s.
f64 gpu_ext_point_seconds(const BenchParams* p);
// Fills next[0..n) with a random single cycle over 0..n-1 (Sattolo's algorithm, fixed seed).
void gpu_ext_make_chain(u32* next, u32 n);

// ---- Pipeline creation (pso.hlsl) -----------------------------------------------------------------------
// Compute (cs_pso) and graphics (vs_pso + ps_pso, RGBA8 + D32, alpha blend) pipelines. "Cold": every
// pipeline is unique (new seed), created without an application cache, so the driver compiles it.
// "Cached": the same pipelines again from the application's cache filled by the cold pass (Vulkan
// VkPipelineCache, D3D12 ID3D12PipelineLibrary), serialized and reloaded like on the next game start.
// Each pass creates up to GE_PSO_MAX pipelines or stops after 40 % of the duration.
constexpr u32 GE_PSO_MAX = 128;
u64 gpu_ext_pso_seed();                      // differs per call and per run

// ---- Texture sampling (texture.hlsl) ------------------------------------------------------------------
// Formats: GE_TEX_SIZE^2 texture of random texels / blocks, one mip; a GE_TEX_GRID_W x _H thread grid takes
// GE_TEX_SAMPLES bilinear samples per thread. Aniso: RGBA8 GE_TEX_SIZE^2 with a full mip chain, gradients
// with a 16:1 footprint, sampled bilinear / trilinear / anisotropic 2x..16x.
constexpr u32 GE_TEX_SIZE = 2048;
constexpr u32 GE_TEX_GRID_W = 2048, GE_TEX_GRID_H = 1024;
constexpr u32 GE_TEX_SAMPLES = 128, GE_ANISO_SAMPLES = 32;
constexpr u32 GE_ANISO_RATIO = 16;

enum GeTexFormat : u8 {
    GF_RGBA8, GF_R32F, GF_RGBA16F, GF_RGBA32F, GF_BC1, GF_BC3, GF_BC4, GF_BC5, GF_BC6H, GF_BC7, GF_COUNT
};
extern const char* const GE_TEX_FORMAT_NAMES[GF_COUNT];
// Bytes of one 4x4 block (BC) or of one texel (others), and whether the format is block compressed.
u32 gpu_ext_tex_unit_bytes(GeTexFormat f, bool* block);
// Random texel / block data for a size x size level (floats in 0..1 for the float formats).
void gpu_ext_tex_fill(GeTexFormat f, u8* dst, u32 size, u32 seed);

constexpr u32 GE_ANISO_MODES = 6;            // bilinear, trilinear, aniso 2x, 4x, 8x, 16x (sampler s0..s5)
extern const char* const GE_ANISO_NAMES[GE_ANISO_MODES];

// Frequently used per-phase helper: live progress over [p0, p1].
void gpu_ext_progress(BenchLive* live, f64 elapsed, f64 total, f32 p0, f32 p1);

// ---- Backend entry points (false = not one of these tests, the base backend runs it) ----------------
bool gpu_vk_run_ext(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                    const CancelFlag* cancel);
bool gpu_d3d12_run_ext(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                       const CancelFlag* cancel);
