// Mesh shader feature test: the same scene drawn by the classic vertex pipeline and by task (amplification)
// + mesh shaders that cull meshlets (frustum + normal cone) before any vertex work. Constants: gpu_features.h.
//
// Scene: FM_OBJECTS bumpy spheres scattered around a camera that turns in place. Each sphere is a grid of
// FM_PATCH_U x FM_PATCH_V meshlets; a meshlet is 8 x 8 quads = 81 vertices, 128 triangles. Positions are
// generated from (object, meshlet, vertex) only, so both pipelines produce identical triangles.
// Raster state for both: back-face culling (front = clockwise, as D3D12's default), depth D32 LESS.
// pc.f.x = time (s), pc.f.y = aspect ratio.
#include "common.hlsli"

static const uint PATCH_U = 16, PATCH_V = 8, MESHLETS = PATCH_U * PATCH_V;   // = FM_PATCH_U / FM_PATCH_V
static const uint SIDE = 9, VERTS = SIDE * SIDE, TRIS = 128;
static const float ZN = 0.25f, ZF = 400.0f, FOCAL = 1.7320508f;               // 60 degree vertical FOV
static const float BUMP = 0.06f;

struct Object {
    float3 center;
    float radius;
    float2 freq;
    float2 phase;
};

Object get_object(uint i) {
    Object o;
    const float a = hash01(i) * 6.2831853f, d = 18.0f + hash01(i * 3u + 1u) * 110.0f;
    o.center = float3(cos(a) * d, (hash01(i * 5u + 2u) - 0.5f) * 50.0f, sin(a) * d);
    o.radius = 1.5f + 2.5f * hash01(i * 7u + 3u);
    o.freq = float2(2.0f + floor(hash01(i * 11u + 4u) * 3.0f), 2.0f + floor(hash01(i * 13u + 5u) * 3.0f));
    o.phase = float2(hash01(i * 17u + 6u), hash01(i * 19u + 7u)) * 6.2831853f;
    return o;
}

float3 sphere_dir(float u, float v) {     // u, v in 0..1 over the whole sphere
    const float phi = u * 6.2831853f, theta = v * 3.1415927f;
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

float3 meshlet_vertex(Object o, uint meshlet, uint local, out float3 n) {
    const float u = ((meshlet % PATCH_U) * 8u + local % SIDE) / (PATCH_U * 8.0f);
    const float v = ((meshlet / PATCH_U) * 8u + local / SIDE) / (PATCH_V * 8.0f);
    n = sphere_dir(u, v);
    const float bump = 1.0f + BUMP * sin(u * 6.2831853f * o.freq.x + o.phase.x) * sin(v * 3.1415927f * o.freq.y + o.phase.y);
    return o.center + n * (o.radius * bump);
}

float3 to_view(float3 world) {            // camera at the origin, turning around Y
    const float yaw = pc.f.x * 0.15f, s = sin(yaw), c = cos(yaw);
    return float3(c * world.x - s * world.z, world.y, s * world.x + c * world.z);
}

float4 project(float3 vp) {
    return float4(vp.x * FOCAL / pc.f.y, vp.y * FOCAL, vp.z * ZF / (ZF - ZN) - ZN * ZF / (ZF - ZN), vp.z);
}

struct MeshVertex {
    float4 pos : SV_Position;
    float3 n : NORMAL;
    nointerpolation float3 col : COLOR0;
};

MeshVertex make_vertex(uint object, uint meshlet, uint local) {
    const Object o = get_object(object);
    float3 n;
    const float3 p = meshlet_vertex(o, meshlet, local, n);
    MeshVertex v;
    v.pos = project(to_view(p));
    v.n = n;
    v.col = float3(0.3f + 0.7f * hash01(object + 101u), 0.3f + 0.7f * hash01(object + 211u), 0.3f + 0.7f * hash01(object + 307u));
    return v;
}

float4 ps_mesh(MeshVertex i) : SV_Target {
    const float3 l = normalize(float3(0.4f, 0.8f, -0.45f));
    return float4(i.col * (0.2f + 0.8f * saturate(dot(normalize(i.n), l))), 1.0f);
}

// ---- classic: DrawIndexedInstanced(FM_INDICES, FM_OBJECTS), 16-bit index buffer from gpu_features.cpp ----
// (vertex id = meshlet * 81 + local vertex; all meshlets of one sphere in one draw instance)
MeshVertex vs_mesh(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    return make_vertex(inst, vid / VERTS, vid % VERTS);
}

// ---- task + mesh: DispatchMesh(FM_OBJECTS * MESHLETS / 32) task groups -----------------------------
struct Payload {
    uint meshlets[32];      // object << 8 | meshlet
};

groupshared Payload payload;
groupshared uint visible_count;

bool meshlet_visible(uint object, uint meshlet) {
    const Object o = get_object(object);
    const float u = ((meshlet % PATCH_U) + 0.5f) / PATCH_U, v = ((meshlet / PATCH_U) + 0.5f) / PATCH_V;
    const float3 axis = sphere_dir(u, v);
    const float3 center = o.center + axis * o.radius;
    const float r = o.radius * 0.45f;                    // bounds an 8 x 8 quad patch incl. the bumps
    // Frustum (view space, symmetric): side planes through the eye, near plane.
    const float3 c = to_view(center);
    const float th = pc.f.y / FOCAL, tv = 1.0f / FOCAL;
    const float kh = rsqrt(1.0f + th * th), kv = rsqrt(1.0f + tv * tv);
    if (c.z < ZN - r) return false;
    if ((abs(c.x) - c.z * th) * kh > r || (abs(c.y) - c.z * tv) * kv > r) return false;
    // Normal cone: every triangle faces away when the view direction is within the cone's back side.
    const float dist = length(center);
    return dot(center / dist, axis) < 0.57f + r / dist;  // sin(35 degree) cone + bounds
}

[numthreads(32, 1, 1)]
void as_mesh(uint gid : SV_GroupID, uint tid : SV_GroupThreadID) {
    const uint object = gid / (MESHLETS / 32u), meshlet = (gid % (MESHLETS / 32u)) * 32u + tid;
    if (tid == 0) visible_count = 0;
    GroupMemoryBarrierWithGroupSync();
    if (meshlet_visible(object, meshlet)) {     // compact the survivors (order doesn't matter)
        uint slot;
        InterlockedAdd(visible_count, 1u, slot);
        payload.meshlets[slot] = object << 8 | meshlet;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visible_count, 1, 1, payload);
}

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void ms_mesh(uint gid : SV_GroupID, uint tid : SV_GroupThreadID, in payload Payload pl,
             out vertices MeshVertex verts[VERTS], out indices uint3 tris[TRIS]) {
    const uint packed = pl.meshlets[gid];
    SetMeshOutputCounts(VERTS, TRIS);
    if (tid < VERTS) verts[tid] = make_vertex(packed >> 8, packed & 255u, tid);
    const uint q = tid >> 1, a = (q / 8u) * SIDE + q % 8u;
    tris[tid] = (tid & 1u) ? uint3(a + 1u, a + SIDE + 1u, a + SIDE) : uint3(a, a + 1u, a + SIDE);
}
