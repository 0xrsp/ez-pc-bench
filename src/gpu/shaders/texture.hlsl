// Texture sampling rate per format and the cost of anisotropic filtering. Workloads: gpu_ext.h.
// Samplers (all wrap): s0 bilinear (mip point), s1 trilinear, s2..s5 anisotropic 2x / 4x / 8x / 16x.
#include "common.hlsli"

Texture2D<float4> tex : register(t0);
SamplerState s_bilinear : register(s0);
SamplerState s_trilinear : register(s1);
SamplerState s_aniso2 : register(s2);
SamplerState s_aniso4 : register(s3);
SamplerState s_aniso8 : register(s4);
SamplerState s_aniso16 : register(s5);
RWByteAddressBuffer out_buf : register(u0);

// One thread per texel of a 2D grid; each takes pc.u.x bilinear samples, moving a fraction of a texel each
// time (cache-friendly: measures the filtering rate, not memory bandwidth). pc.f.x = 1 / texture size.
[numthreads(8, 8, 1)]
void cs_tex_sample(uint2 id : SV_DispatchThreadID) {
    float2 uv = (float2(id) + 0.5f) * pc.f.x;
    const float2 step = float2(0.37f, 0.21f) * pc.f.x;
    float4 acc = 0;
    [unroll(8)] for (uint i = 0; i < pc.u.x; ++i) {
        acc += tex.SampleLevel(s_bilinear, uv, 0.0f);
        uv += step;
    }
    if (dot(acc, 1.0f) == -1.0f) out_buf.Store((id.x & 255) * 4, 1u);
}

// Same pattern with explicit gradients: 16:1 anisotropic footprint (ddx = pc.f.y, ddy = pc.f.z in uv).
// pc.u.y selects the sampler (0 bilinear, 1 trilinear, 2..5 aniso 2x..16x).
#define ANISO_LOOP(S)                                                       \
    [unroll(4)] for (uint i = 0; i < pc.u.x; ++i) {                         \
        acc += tex.SampleGrad(S, uv, dx, dy);                               \
        uv += step;                                                         \
    }

[numthreads(8, 8, 1)]
void cs_tex_aniso(uint2 id : SV_DispatchThreadID) {
    float2 uv = (float2(id) + 0.5f) * pc.f.x;
    const float2 step = float2(0.37f, 0.21f) * pc.f.x;
    const float2 dx = float2(pc.f.y, 0.0f), dy = float2(0.0f, pc.f.z);
    float4 acc = 0;
    switch (pc.u.y) {
    case 0: ANISO_LOOP(s_bilinear) break;
    case 1: ANISO_LOOP(s_trilinear) break;
    case 2: ANISO_LOOP(s_aniso2) break;
    case 3: ANISO_LOOP(s_aniso4) break;
    case 4: ANISO_LOOP(s_aniso8) break;
    default: ANISO_LOOP(s_aniso16) break;
    }
    if (dot(acc, 1.0f) == -1.0f) out_buf.Store((id.x & 255) * 4, 1u);
}
