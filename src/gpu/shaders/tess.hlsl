// Tessellation feature test: a FT_GRID x FT_GRID grid of quad patches (4 control points each, generated from
// SV_VertexID), tessellated with a uniform integer factor and displaced into a terrain by the domain shader.
// Draw(FT_GRID^2 * 4) as a 4-control-point patch list, no culling, depth D32 LESS. Triangles per patch with
// factor F on all edges and inside: 2 * F^2.
// pc.u.x = patches per row, pc.f.x = time (s), pc.f.y = aspect, pc.f.z = tessellation factor, pc.f.w = patch size.
#include "common.hlsli"

static const float ZN = 0.25f, ZF = 600.0f, FOCAL = 1.7320508f;

struct ControlPoint {
    float2 xz : POSITION;
};

ControlPoint vs_tess(uint vid : SV_VertexID) {
    const uint patch = vid / 4u, corner = vid % 4u, n = pc.u.x;
    const float2 cell = float2(patch % n + (corner & 1u), patch / n + (corner >> 1));
    ControlPoint o;
    o.xz = (cell - n * 0.5f) * pc.f.w;
    return o;
}

struct PatchFactors {
    float edges[4] : SV_TessFactor;
    float inside[2] : SV_InsideTessFactor;
};

PatchFactors hs_factors(InputPatch<ControlPoint, 4> ip) {
    PatchFactors f;
    f.edges[0] = f.edges[1] = f.edges[2] = f.edges[3] = pc.f.z;
    f.inside[0] = f.inside[1] = pc.f.z;
    return f;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_factors")]
[maxtessfactor(64.0)]
ControlPoint hs_tess(InputPatch<ControlPoint, 4> ip, uint i : SV_OutputControlPointID) { return ip[i]; }

float height(float2 p, float t) {
    float h = 0.0f, a = 6.0f, f = 0.021f;
    [unroll] for (uint k = 0; k < 5; ++k) {
        h += a * sin(p.x * f + k * 1.7f + t * 0.3f) * cos(p.y * f * 1.13f - k * 0.9f);
        a *= 0.45f;
        f *= 2.17f;
    }
    return h;
}

struct TessOut {
    float4 pos : SV_Position;
    float3 n : NORMAL;
    float h : TEXCOORD0;
};

[domain("quad")]
TessOut ds_tess(PatchFactors pf, float2 uv : SV_DomainLocation, const OutputPatch<ControlPoint, 4> op) {
    const float2 xz = lerp(lerp(op[0].xz, op[1].xz, uv.x), lerp(op[2].xz, op[3].xz, uv.x), uv.y);
    const float t = pc.f.x, e = 0.1f;
    const float h = height(xz, t);
    const float3 n = normalize(float3(h - height(xz + float2(e, 0.0f), t), e, h - height(xz + float2(0.0f, e), t)));
    // Camera gliding over the terrain, pitched down ~25 degrees.
    float3 vp = float3(xz.x - sin(t * 0.1f) * 20.0f, h - 34.0f, xz.y + 125.0f);
    const float cp = cos(0.45f), sp = sin(0.45f);
    vp = float3(vp.x, cp * vp.y + sp * vp.z, -sp * vp.y + cp * vp.z);
    TessOut o;
    o.pos = float4(vp.x * FOCAL / pc.f.y, vp.y * FOCAL, vp.z * ZF / (ZF - ZN) - ZN * ZF / (ZF - ZN), vp.z);
    o.n = n;
    o.h = h;
    return o;
}

float4 ps_tess(TessOut i) : SV_Target {
    const float3 l = normalize(float3(0.5f, 0.7f, -0.4f));
    const float3 albedo = lerp(float3(0.25f, 0.40f, 0.20f), float3(0.75f, 0.72f, 0.68f), saturate(i.h * 0.08f + 0.4f));
    return float4(albedo * (0.2f + 0.8f * saturate(dot(normalize(i.n), l))), 1.0f);
}
