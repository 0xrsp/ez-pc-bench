// Variable-rate shading feature test: FV_LAYERS opaque full-screen layers (vs_fullscreen from raster.hlsl,
// Draw(3, FV_LAYERS)) of an ALU-heavy procedural terrain shader, so the pixel-shader rate decides the
// frame time. Drawn at 1x1, at a per-draw 2x2 rate and with a screen-space rate image.
// pc.f.x = time (s), pc.f.y = aspect ratio.
#include "common.hlsli"

struct FullscreenOut {      // = raster.hlsl
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint layer : TEXCOORD1;
};

// Float-only hash and value noise (32-bit integer multiplies are slow on some GPUs, e.g. RDNA3).
float hash2(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float vnoise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(hash2(i), hash2(i + float2(1, 0)), u.x), lerp(hash2(i + float2(0, 1)), hash2(i + float2(1, 1)), u.x), u.y);
}

float fbm(float2 p) {
    float v = 0.0f, a = 0.5f;
    [unroll] for (uint k = 0; k < 8; ++k) {
        v += a * vnoise(p);
        p = float2(1.6f * p.x - 1.2f * p.y, 1.2f * p.x + 1.6f * p.y);
        a *= 0.5f;
    }
    return v;
}

float4 ps_vrs(FullscreenOut i) : SV_Target {
    const float2 p = (i.uv * float2(pc.f.y, 1.0f)) * 4.0f + float2(pc.f.x * 0.07f, 0.0f) + i.layer * 13.7f;
    const float e = 0.004f;
    const float h = fbm(p), hx = fbm(p + float2(e, 0.0f)), hy = fbm(p + float2(0.0f, e));
    const float3 n = normalize(float3((h - hx) * 2.0f, (h - hy) * 2.0f, e));
    const float3 l = normalize(float3(-0.5f, 0.6f, 0.6f));
    const float3 water = float3(0.10f, 0.25f, 0.45f), grass = float3(0.25f, 0.45f, 0.18f), rock = float3(0.55f, 0.50f, 0.45f);
    float3 albedo = h < 0.45f ? water : lerp(grass, rock, saturate((h - 0.5f) * 4.0f));
    albedo *= 0.85f + 0.3f * vnoise(p * 23.0f);
    return float4(albedo * (0.25f + 0.75f * saturate(dot(n, l))), 1.0f);
}
