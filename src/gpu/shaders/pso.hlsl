// Pipeline-creation test: game-like vertex / pixel shaders and a compute shader of typical size. The test only
// creates pipelines from them (never draws). Every pipeline must be new to the driver's caches: Vulkan varies
// the SEED specialization constant, D3D12 varies the shader hash in a copy of the DXIL container (gpu_ext.h).
#include "common.hlsli"

#ifdef __spirv__
[[vk::constant_id(0)]] const uint SEED = 0;
#else
static const uint SEED = 0;
#endif

Texture2D<float4> t_albedo : register(t0);
Texture2D<float4> t_normal : register(t1);
Texture2D<float4> t_material : register(t2);
Texture2D<float4> t_shadow : register(t3);
SamplerState s_linear : register(s0);
RWByteAddressBuffer out_buf : register(u0);

static const float PI = 3.14159265f;
static const uint LIGHTS = 8;

struct VsOut {
    float4 pos : SV_Position;
    float3 wpos : WORLDPOS;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float4 shadow_pos : TEXCOORD1;
};

float3x3 rotation(float3 axis, float angle) {
    float s, c;
    sincos(angle, s, c);
    const float t = 1.0f - c;
    return float3x3(t * axis.x * axis.x + c, t * axis.x * axis.y - s * axis.z, t * axis.x * axis.z + s * axis.y,
                    t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c, t * axis.y * axis.z - s * axis.x,
                    t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x, t * axis.z * axis.z + c);
}

float4x4 perspective(float fov, float aspect, float zn, float zf) {
    const float y = 1.0f / tan(fov * 0.5f), x = y / aspect;
    return float4x4(x, 0, 0, 0, 0, y, 0, 0, 0, 0, zf / (zf - zn), -zn * zf / (zf - zn), 0, 0, 1, 0);
}

// Procedural instanced mesh: a displaced sphere per instance, skinned by two "bones".
VsOut vs_pso(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const uint ring = vid / 64, seg = vid % 64;
    const float theta = ring * (PI / 32.0f), phi = seg * (2.0f * PI / 64.0f);
    float3 p = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
    float3 n = p;
    const float h = hash01(iid * 7919u + vid) * 0.05f + sin(phi * 6.0f + pc.f.x) * 0.03f;
    p *= 1.0f + h;
    const float3x3 bone0 = rotation(normalize(float3(0.3f, 1.0f, 0.1f)), pc.f.x + iid * 0.1f);
    const float3x3 bone1 = rotation(normalize(float3(1.0f, 0.2f, 0.4f)), pc.f.x * 0.7f - iid * 0.05f);
    const float w = saturate(p.y * 0.5f + 0.5f);
    p = lerp(mul(bone0, p), mul(bone1, p), w);
    n = normalize(lerp(mul(bone0, n), mul(bone1, n), w));
    const float3 offset = float3((iid % 16) * 2.5f - 20.0f, (iid / 16 % 16) * 2.5f - 20.0f, 30.0f + (iid / 256) * 3.0f);
    VsOut o;
    o.wpos = p + offset;
    o.normal = n;
    const float3 t = normalize(cross(n, float3(0.0f, 1.0f, 0.001f)));
    o.tangent = float4(t, (iid & 1) ? 1.0f : -1.0f);
    o.uv = float2(phi / (2.0f * PI), theta / PI) * (1.0f + (float)(SEED & 0xFFFFu) * 1e-7f);
    o.pos = mul(perspective(1.0f, pc.f.y, 0.1f, 200.0f), float4(o.wpos, 1.0f));
    o.shadow_pos = float4(o.wpos.xy * 0.02f + 0.5f, o.wpos.z * 0.01f, 1.0f);
    return o;
}

float d_ggx(float nh, float a) {
    const float a2 = a * a, d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d);
}

float v_smith(float nv, float nl, float a) {
    const float k = a * 0.5f;
    return 0.25f / ((nv * (1.0f - k) + k) * (nl * (1.0f - k) + k));
}

float3 f_schlick(float3 f0, float vh) { return f0 + (1.0f - f0) * pow(1.0f - vh, 5.0f); }

float shadow_pcf(float4 sp) {
    float s = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float d = t_shadow.SampleLevel(s_linear, sp.xy + float2(x, y) / 2048.0f, 0.0f).r;
            s += sp.z <= d + 0.002f ? 1.0f : 0.0f;
        }
    return s / 9.0f;
}

float3 aces(float3 x) { return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f)); }

// PBR shading: normal map, 8 point lights (GGX + Smith + Schlick), 3x3 PCF shadow, fog, ACES tone map.
float4 ps_pso(VsOut i) : SV_Target {
    const float4 albedo = t_albedo.Sample(s_linear, i.uv);
    const float3 tn = t_normal.Sample(s_linear, i.uv * 2.0f).xyz * 2.0f - 1.0f;
    const float4 mat = t_material.Sample(s_linear, i.uv);
    const float3 nrm = normalize(i.normal), tan = normalize(i.tangent.xyz);
    const float3 bit = cross(nrm, tan) * i.tangent.w;
    const float3 n = normalize(tn.x * tan + tn.y * bit + tn.z * nrm);
    const float3 v = normalize(-i.wpos);
    const float rough = max(mat.g, 0.05f), metal = mat.b;
    const float3 f0 = lerp(0.04f, albedo.rgb, metal);
    const float nv = max(dot(n, v), 1e-4f);
    float3 color = 0.0f;
    [loop] for (uint l = 0; l < LIGHTS; ++l) {
        const float3 lp = float3(sin(l * 1.7f + pc.f.x) * 20.0f, cos(l * 2.3f) * 10.0f, 25.0f + l * 3.0f);
        const float3 lc = float3(hash01(l * 3u + SEED), hash01(l * 3u + 1u), hash01(l * 3u + 2u)) * 40.0f;
        const float3 d = lp - i.wpos;
        const float dist2 = dot(d, d);
        const float3 ld = d * rsqrt(dist2);
        const float3 h = normalize(ld + v);
        const float nl = saturate(dot(n, ld)), nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
        const float3 f = f_schlick(f0, vh);
        const float3 spec = d_ggx(nh, rough * rough) * v_smith(nv, nl, rough * rough) * f;
        const float3 diff = (1.0f - f) * (1.0f - metal) * albedo.rgb / PI;
        color += (diff + spec) * lc * nl / (1.0f + dist2);
    }
    color *= 0.3f + 0.7f * shadow_pcf(i.shadow_pos);
    color += albedo.rgb * mat.r * 0.03f;
    const float fog = 1.0f - exp(-length(i.wpos) * 0.01f);
    color = lerp(color, float3(0.5f, 0.6f, 0.7f), fog);
    return float4(pow(aces(color), 1.0f / 2.2f), albedo.a);
}

// Compute: fractal value noise (6 octaves) + a small separable blur from groupshared memory.
groupshared float g_tile[64 + 8];

float value_noise(float2 p) {
    const float2 i = floor(p), f = frac(p), u = f * f * (3.0f - 2.0f * f);
    const uint ix = (uint)(int)i.x, iy = (uint)(int)i.y;
    const float a = hash01(ix * 73856093u ^ iy * 19349663u ^ SEED), b = hash01((ix + 1u) * 73856093u ^ iy * 19349663u);
    const float c = hash01(ix * 73856093u ^ (iy + 1u) * 19349663u), d = hash01((ix + 1u) * 73856093u ^ (iy + 1u) * 19349663u);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

[numthreads(64, 1, 1)]
void cs_pso(uint tid : SV_DispatchThreadID, uint li : SV_GroupIndex) {
    float2 p = float2(tid % 1024, tid / 1024) * 0.01f + pc.f.xy;
    float sum = 0.0f, amp = 0.5f;
    [unroll] for (uint o = 0; o < 6; ++o) {
        sum += value_noise(p) * amp;
        p = mul(float2x2(1.6f, 1.2f, -1.2f, 1.6f), p);
        amp *= 0.5f;
    }
    g_tile[li + 4] = sum;
    if (li < 4) { g_tile[li] = sum; g_tile[li + 68] = sum; }
    GroupMemoryBarrierWithGroupSync();
    float blur = 0.0f;
    [unroll] for (int k = -4; k <= 4; ++k) blur += g_tile[li + 4 + k] * (1.0f - abs(k) / 5.0f);
    out_buf.Store(tid * 4, asuint(blur));
}
