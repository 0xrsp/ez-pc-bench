// Preview: bilinear downscale of the render target (t0) into the preview texture (u0).
// pc.u.xy = preview size. Dispatch ceil(w / 8) x ceil(h / 8).
#include "common.hlsli"

Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
[[vk::image_format("rgba8")]] RWTexture2D<float4> dst : register(u0);

[numthreads(8, 8, 1)]
void cs_downscale(uint2 id : SV_DispatchThreadID) {
    if (any(id >= pc.u.xy)) return;
    const float2 uv = (float2(id) + 0.5f) / float2(pc.u.xy);
    dst[id] = float4(src.SampleLevel(samp, uv, 0).rgb, 1.0f);
}
