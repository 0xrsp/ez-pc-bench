// 3D / 2D raster tests. No vertex buffers: all geometry is generated from SV_VertexID / SV_InstanceID.
// Pipeline state shared by every test: RGBA8_UNORM target, no culling, triangle list.
// Each section lists what the host must put in pc (push / root constants).
#include "common.hlsli"

SamplerState samp : register(s0);   // linear filter, wrap

// ---- triangle throughput / draw calls ------------------------------------------------------------
// vs_triangles: Draw(GR_TRI_COUNT * 3). pc.u.x = grid columns, pc.u.y = grid rows,
//               pc.f.xy = cell size (NDC), pc.f.zw = triangle size (NDC). Host: cols = width / 4,
//               rows = height / 4, cell = 2 / cols|rows, triangle = 2 px. Triangles wrap over the grid.
// vs_drawcall:  GR_DRAWS separate Draw(3) per frame, pc.u.x = draw index (changes per draw), rest as above
//               (grid columns are recovered from the cell size: cols = 2 / pc.f.x).
struct ColorOut {
    float4 pos : SV_Position;
    float4 col : COLOR0;
};

ColorOut tiny_triangle(uint tri, uint corner, uint cols) {
    const uint cells = cols * pc.u.y;
    const uint cell = tri % cells;
    float2 p = float2(cell % cols, cell / cols) * pc.f.xy - 1.0f;
    if (corner == 1) p.x += pc.f.z;
    if (corner == 2) p.y += pc.f.w;
    ColorOut o;
    o.pos = float4(p, 0.5f, 1.0f);
    o.col = float4(hash01(tri), hash01(tri * 7u + 1u), hash01(tri * 13u + 2u), 1.0f);
    return o;
}

ColorOut vs_triangles(uint vid : SV_VertexID) { return tiny_triangle(vid / 3u, vid % 3u, pc.u.x); }
ColorOut vs_drawcall(uint vid : SV_VertexID) { return tiny_triangle(pc.u.x * 37u, vid % 3u, (uint)(2.0f / pc.f.x + 0.5f)); }
float4 ps_color(ColorOut i) : SV_Target { return i.col; }

// ---- full-screen layers (pixel fill, texture fill) ------------------------------------------------
// vs_fullscreen: Draw(3, layers) — one full-screen triangle per instance.
// ps_fill:    pc.u.x = frame index. Opaque, per-pixel varying color (defeats color compression).
// ps_texfill: t0..t3 = GR_TEX_SIZE^2 RGBA8 noise textures. pc.f.xy = uv scale (width, height) / GR_TEX_SIZE,
//             pc.f.zw = per-frame uv offset. 4 bilinear samples per pixel per layer.
struct FullscreenOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint layer : TEXCOORD1;
};

FullscreenOut vs_fullscreen(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    FullscreenOut o;
    o.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.5f, 1.0f);
    o.uv = uv;
    o.layer = inst;
    return o;
}

float4 ps_fill(FullscreenOut i) : SV_Target {
    const uint2 p = uint2(i.pos.xy);
    const uint h = hash_u32(p.x * 7919u ^ p.y * 104729u ^ i.layer * 0x9e3779b9u ^ pc.u.x);
    return float4((h & 255u) / 255.0f, ((h >> 8) & 255u) / 255.0f, ((h >> 16) & 255u) / 255.0f, 1.0f);
}

Texture2D<float4> tex0 : register(t0);
Texture2D<float4> tex1 : register(t1);
Texture2D<float4> tex2 : register(t2);
Texture2D<float4> tex3 : register(t3);

float4 ps_texfill(FullscreenOut i) : SV_Target {
    const float2 uv = i.uv * pc.f.xy + pc.f.zw + i.layer * 0.173f;
    return (tex0.Sample(samp, uv) + tex1.Sample(samp, uv * 1.07f + 0.31f) +
            tex2.Sample(samp, uv * 0.93f + 0.57f) + tex3.Sample(samp, uv * 1.13f + 0.83f)) * 0.25f;
}

// ---- lit scene ------------------------------------------------------------------------------------
// vs_scene: Draw(36, GR_SCENE_CUBES) with a D32_FLOAT depth buffer (clear 1.0, test LESS, write on).
// pc.f.x = time in seconds (rotation), pc.f.y = aspect ratio (width / height).
struct SceneOut {
    float4 pos : SV_Position;
    float3 n : NORMAL;
    float3 col : COLOR0;
};

static const float3 FACE_N[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
static const float3 FACE_U[6] = { float3(0, 0, 1), float3(0, 0, -1), float3(1, 0, 0), float3(1, 0, 0), float3(-1, 0, 0), float3(1, 0, 0) };
static const float2 QUAD[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };

float3 rotate_y(float3 v, float a) {
    const float s = sin(a), c = cos(a);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

SceneOut vs_scene(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    const uint face = vid / 6u;
    const float3 n = FACE_N[face], u = FACE_U[face], v = cross(n, u);
    const float2 q = QUAD[vid % 6u];
    const float spin = pc.f.x * (0.5f + hash01(inst));
    const float3 local = rotate_y((n + q.x * u + q.y * v) * 0.8f, spin);

    // 200 x 100 grid on the ground plane, gentle height waves.
    const float gx = (float)(inst % 200u), gz = (float)(inst / 200u);
    const float3 center = float3((gx - 100.0f) * 3.0f, sin(gx * 0.3f + pc.f.x) * 2.0f + cos(gz * 0.2f) * 2.0f, gz * 3.0f + 8.0f);
    const float3 world = local + center;

    // Camera at (0, 40, -20), pitched down ~35 degrees, 60 degree vertical FOV.
    float3 vp = world - float3(0.0f, 40.0f, -20.0f);
    const float cp = cos(0.6f), sp = sin(0.6f);
    vp = float3(vp.x, cp * vp.y - sp * vp.z, sp * vp.y + cp * vp.z);
    const float f = 1.0f / tan(0.5236f), zn = 0.5f, zf = 1000.0f;

    SceneOut o;
    o.pos = float4(vp.x * f / pc.f.y, vp.y * f, vp.z * zf / (zf - zn) - zn * zf / (zf - zn), vp.z);
    o.n = rotate_y(n, spin);
    o.col = float3(0.3f + 0.7f * hash01(inst), 0.3f + 0.7f * hash01(inst + 101u), 0.3f + 0.7f * hash01(inst + 211u));
    return o;
}

float4 ps_scene(SceneOut i) : SV_Target {
    const float3 l = normalize(float3(0.5f, 1.0f, -0.3f));
    const float diffuse = saturate(dot(normalize(i.n), l));
    return float4(i.col * (0.25f + 0.75f * diffuse), 1.0f);
}

// ---- sprites (2D) ---------------------------------------------------------------------------------
// vs_sprite: Draw(6, GR_SPRITES), alpha blending (src alpha, 1 - src alpha). t0 = GR_SPRITE_TEX^2 sprite
// with an alpha disc. pc.f.xy = sprite size in NDC (32 px), pc.f.z = time in seconds.
struct SpriteOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

Texture2D<float4> sprite_tex : register(t0);

SpriteOut vs_sprite(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    const float2 q = QUAD[vid % 6u];
    const float2 base = float2(hash01(inst), hash01(inst ^ 0x9e3779b9u)) * 2.0f - 1.0f;
    const float2 drift = float2(sin(pc.f.z + inst * 0.37f), cos(pc.f.z * 1.3f + inst * 0.11f)) * 0.05f;
    SpriteOut o;
    o.pos = float4(base + drift + q * pc.f.xy * 0.5f, 0.5f, 1.0f);
    o.uv = q * 0.5f + 0.5f;
    o.col = float4(0.4f + 0.6f * hash01(inst + 7u), 0.4f + 0.6f * hash01(inst + 13u), 0.4f + 0.6f * hash01(inst + 29u), 0.85f);
    return o;
}

float4 ps_sprite(SpriteOut i) : SV_Target { return sprite_tex.Sample(samp, i.uv) * i.col; }
