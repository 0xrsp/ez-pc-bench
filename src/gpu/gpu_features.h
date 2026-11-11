#pragma once
// 3DMark-style feature tests (GPU_TESTS group "Features"): ray tracing, mesh shaders vs the vertex pipeline,
// tessellation, variable-rate shading, sampler feedback (D3D12 only) and async compute overlap.
// Backends: gpu_d3d12_features.cpp and gpu_vk_features.cpp, same shaders (rt / mesh / tess / vrs / sfb.hlsl)
// and the same procedural scenes from gpu_features.cpp, so both APIs run identical work. Every test renders
// offscreen and publishes the usual preview (gpu_common.h). Unsupported features are skipped with a reason.
#include "gpu/gpu_common.h"

struct D3d;
struct VkCtx;

inline bool gpu_is_feature_test(GpuTestId t) { return t >= GT_RAYTRACING && t <= GT_ASYNC_COMPUTE; }

// ---- ray tracing (rt.hlsl) ------------------------------------------------------------------------
// Terrain: FR_GRID^2 cells (2 triangles each), indexed, float3 vertices rewritten by cs_rt_terrain every
// frame, BLAS built with ALLOW_UPDATE and refitted every frame. Spheres: one UV sphere BLAS, FR_SPHERES
// instances. TLAS (terrain + spheres) rebuilt every frame from CPU-written instance descriptors.
constexpr u32 FR_GRID = 256;
constexpr u32 FR_GRID_VERTS = FR_GRID + 1;
constexpr u32 FR_TERRAIN_VERTS = FR_GRID_VERTS * FR_GRID_VERTS;
constexpr u32 FR_TERRAIN_TRIS = FR_GRID * FR_GRID * 2;
constexpr f32 FR_TERRAIN_SIZE = 220.0f;
constexpr u32 FR_SPHERE_SEG = 32, FR_SPHERE_RINGS = 16;
constexpr u32 FR_SPHERE_VERTS = (FR_SPHERE_SEG + 1) * (FR_SPHERE_RINGS + 1);
constexpr u32 FR_SPHERE_TRIS = FR_SPHERE_SEG * FR_SPHERE_RINGS * 2;
constexpr u32 FR_SPHERES = 2048;
constexpr u32 FR_INSTANCES = FR_SPHERES + 1;    // instance 0 = terrain
constexpr u32 FR_BLAS_BUILDS = 8;               // timed full builds of the terrain BLAS during setup

// Same layout as D3D12_RAYTRACING_INSTANCE_DESC and VkAccelerationStructureInstanceKHR (64 bytes).
struct FeatInstance {
    f32 transform[12];      // 3x4 row-major object -> world
    u32 id_mask;            // InstanceID (24 bits) | mask << 24
    u32 offset_flags;       // hit group offset (24 bits) | flags << 24
    u64 blas;               // GPU address of the BLAS
};
static_assert(sizeof(FeatInstance) == 64, "instance descriptor layout");

void feat_terrain_indices(u32* idx);                // FR_TERRAIN_TRIS * 3
void feat_sphere_mesh(f32* xyz, u32* idx);          // FR_SPHERE_VERTS * 3 floats, FR_SPHERE_TRIS * 3 indices
void feat_rt_instances(FeatInstance* inst, f32 time, u64 terrain_blas, u64 sphere_blas);   // FR_INSTANCES

// ---- mesh shaders (mesh.hlsl) ------------------------------------------------------------------------
constexpr u32 FM_OBJECTS = 1024;
constexpr u32 FM_MESHLETS = 16 * 8;             // per object (FM_PATCH_U x FM_PATCH_V in mesh.hlsl)
constexpr u32 FM_MESHLET_VERTS = 81, FM_MESHLET_TRIS = 128;
constexpr u32 FM_INDICES = FM_MESHLETS * FM_MESHLET_TRIS * 3;       // per object (classic index buffer, u16)
constexpr u32 FM_TASK_GROUPS = FM_OBJECTS * FM_MESHLETS / 32;
constexpr f64 FM_TRIS_PER_FRAME = (f64)FM_OBJECTS * FM_MESHLETS * FM_MESHLET_TRIS;
void feat_mesh_indices(u16* idx);                   // FM_INDICES

// ---- tessellation (tess.hlsl) ----------------------------------------------------------------------
constexpr u32 FT_GRID = 48;                         // patches per side
constexpr f32 FT_FACTOR = 32.0f;                    // edge and inside factors (integer partitioning)
constexpr f32 FT_PATCH_SIZE = 5.0f;
constexpr f64 FT_TRIS_PER_FRAME = 2.0 * FT_FACTOR * FT_FACTOR * FT_GRID * FT_GRID;

// ---- variable-rate shading (vrs.hlsl) -----------------------------------------------------------------
constexpr u32 FV_LAYERS = 2;
// Rate image texels use the same encoding in D3D12 (D3D12_SHADING_RATE) and Vulkan (log2(w) << 2 | log2(h)).
constexpr u8 FV_RATE_1X1 = 0x0, FV_RATE_2X2 = 0x5, FV_RATE_4X4 = 0xA;
// Foveated rate image for a w x h target with tile x tile texels: 1x1 in the center, 2x2 around it, 4x4 at
// the edges. rates holds ceil(w / tile) * ceil(h / tile) bytes, row pitch = ceil(w / tile). Returns the
// share of pixels shaded at a reduced rate.
f64 feat_vrs_rate_image(u8* rates, u32 w, u32 h, u32 tile);

// ---- sampler feedback (sfb.hlsl, D3D12 only) ------------------------------------------------------------
constexpr u32 FS_TEX = 2048, FS_MIPS = 12, FS_LAYERS = 16;
constexpr u32 FS_REGION = 64;                       // feedback mip region (texels), map = FS_TEX / FS_REGION
// Procedural RGBA8 texture with a full mip chain, mips packed one after another (tightly, level 0 first).
u64 feat_sfb_texture_bytes();
void feat_sfb_texture(u32* texels);

// ---- async compute ------------------------------------------------------------------------------------
// Graphics = the triangle-throughput frame (vs_triangles + ps_color, GR_TRI_COUNT tiny triangles: raster
// bound). Compute = cs_fp32 over GC_ALU_GROUPS groups (ALU bound) with its iteration count calibrated so it
// takes as long as the graphics frame alone. Serial: both on the graphics queue. Async: compute on a
// compute queue. Overlap gain = serial frame time / async frame time - 1.
constexpr u32 FA_CAL_ITERS = 64;                    // calibration iteration count

// ---- pass statistics ---------------------------------------------------------------------------------------
struct FeatPass {
    f64 avg_ms, p99_ms, p999_ms;
    u32 frames;
};
void feat_pass_begin();
void feat_pass_push(f32 frame_ms);
FeatPass feat_pass_end();
f64 feat_gain_pct(f64 base_ms, f64 feature_ms);     // base / feature - 1, in % (+ = feature pass faster)

// Warm-up for pass i (0 = first pass of the test, full warm-up; later passes are short).
f64 feat_pass_warmup(const BenchParams* p, u32 pass);

// Backends
void d3d_feature_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                      const CancelFlag* cancel);
void vk_feature_test(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                     const CancelFlag* cancel);
