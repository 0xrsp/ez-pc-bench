// Sampler feedback feature test (D3D12 only, DXIL only): FS_LAYERS opaque full-screen layers
// (vs_fullscreen from raster.hlsl, Draw(3, FS_LAYERS)) of a ground plane seen in perspective, textured with a
// mip-mapped FS_TEX^2 texture (t0, trilinear through s0), so every layer touches a range of mips.
// ps_sfb_feedback also records the sampled mips in a MinMip feedback map (u1) paired with t0.
// pc.f.x = time (s), pc.f.y = aspect ratio.
#include "common.hlsli"

struct FullscreenOut {      // = raster.hlsl
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint layer : TEXCOORD1;
};

Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> feedback : register(u1);

// Camera 10 units above the plane y = 0, pitched down 55 degrees (every pixel hits the plane), 60 degree FOV.
float2 ground_uv(FullscreenOut i) {
    const float2 ndc = float2(i.uv.x * 2.0f - 1.0f, 1.0f - i.uv.y * 2.0f) * float2(pc.f.y, 1.0f) * 0.57735f;
    const float cp = cos(0.96f), sp = sin(0.96f);
    const float3 d = float3(ndc.x, ndc.y * cp - sp, ndc.y * sp + cp);
    const float t = 10.0f / -d.y;
    const float a = i.layer * 0.39f + pc.f.x * 0.05f, s = sin(a), c = cos(a);
    const float2 xz = float2(d.x, d.z) * t;
    return float2(c * xz.x - s * xz.y, s * xz.x + c * xz.y) * 0.02f + float2(i.layer * 0.173f, pc.f.x * 0.01f);
}

float4 ps_sfb_plain(FullscreenOut i) : SV_Target {
    return tex.Sample(samp, ground_uv(i));
}

float4 ps_sfb_feedback(FullscreenOut i) : SV_Target {
    const float2 uv = ground_uv(i);
    feedback.WriteSamplerFeedback(tex, samp, uv);
    return tex.Sample(samp, uv);
}
