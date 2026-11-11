#pragma once
// D3D12 side of the extra tests (gpu_ext.h). They use their own root signature (same parameters as the base
// one, plus static samplers s0..s5, see texture.hlsl) and their own PSOs, created and released per test.
// gpu_d3d12_ext.cpp: helpers + compute tests; gpu_d3d12_memory.cpp, gpu_d3d12_pso.cpp, gpu_d3d12_texture.cpp.
#include "gpu/gpu_ext.h"
#include "gpu/gpu_d3d12_internal.h"

ID3D12RootSignature* d3x_root(D3d* d);
ID3D12PipelineState* d3x_compute_pso(D3d* d, ID3D12RootSignature* root, const u8* dxil, u32 size);
D3D_SHADER_MODEL d3x_shader_model(D3d* d);         // highest supported, up to 6.9

struct D3xDispatch {
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
    u32 set;
    GpuParams pc;
    u32 gx, gy;
};

// Warm-up, then timed batches (GPU timestamps around every dispatch, ~20 ms per batch) for `seconds`.
// Dispatch times (ms) go to ms[] (at most max); returns their count, or -1 if the device was removed.
i32 d3x_timed(D3d* d, const D3xDispatch* dp, f64 warm, f64 seconds, f64 work, const char* phase, const char* unit,
              f64 unit_div, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1, f64* ms, u32 max);
void d3x_once(D3d* d, const D3xDispatch* dp);        // one untimed dispatch, waits for it

// Default-heap texture with mips levels, left in a shader-resource state. data holds each level tightly packed
// (level i at offsets[i], rows of size >> i texels or 4x4 blocks).
ID3D12Resource* d3x_texture(D3d* d, DXGI_FORMAT format, u32 size, u32 mips, const u8* data, const u64* offsets);

void d3x_memory_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                     const CancelFlag* cancel);
void d3x_pso_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel);
void d3x_texture_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                      const CancelFlag* cancel);
