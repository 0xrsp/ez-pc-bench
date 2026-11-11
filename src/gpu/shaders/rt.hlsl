// Ray tracing feature test: inline ray queries (DXR 1.1 / VK_KHR_ray_query) from a compute shader.
// Procedural scene (gpu_features.h): a wavy terrain (one BLAS, deformed and refitted every frame) and
// FR_SPHERES instanced spheres (one BLAS) in a TLAS rebuilt every frame.
#include "common.hlsli"

// ---- terrain deform -------------------------------------------------------------------------------
// cs_rt_terrain: u0 = vertex buffer (float3, 12 B), u1 = ray counter (reset to 0 here, before the trace).
// pc.u.x = vertices per row, pc.f.x = time (s), pc.f.y = cell size, pc.f.z = half extent.
// Dispatch ceil(verts_per_row / 8)^2 groups of 8 x 8.
RWByteAddressBuffer vertices : register(u0);
RWByteAddressBuffer counter : register(u1);

float terrain_height(float x, float z, float t) {
    return 2.6f * sin(x * 0.071f + t * 0.9f) * cos(z * 0.083f - t * 0.6f) +
           1.3f * sin((x + z) * 0.17f + t * 1.4f) + 0.6f * cos(x * 0.31f - z * 0.23f);
}

[numthreads(8, 8, 1)]
void cs_rt_terrain(uint2 id : SV_DispatchThreadID) {
    const uint n = pc.u.x;
    if (all(id == 0)) counter.Store(0, 0u);
    if (any(id >= n)) return;
    const float x = id.x * pc.f.y - pc.f.z, z = id.y * pc.f.y - pc.f.z;
    vertices.Store3((id.y * n + id.x) * 12u, asuint(float3(x, terrain_height(x, z, pc.f.x), z)));
}

// ---- trace ----------------------------------------------------------------------------------------
// cs_rt_trace: t0 = TLAS, t1 = terrain indices (u32), t2 = terrain vertices (float3), u0 = output image,
// u1 = ray counter (one u32, rays traced this frame, summed per wave). Instance 0 = terrain, InstanceID
// 1..N = unit spheres (object space radius 1). Per pixel: 1 primary ray; on a hit 1 shadow ray + 2
// ambient-occlusion rays. pc.u.xy = image size, pc.u.z = frame index, pc.f.x = time (s), pc.f.y = aspect.
RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer terrain_idx : register(t1);
ByteAddressBuffer terrain_vtx : register(t2);
[[vk::image_format("rgba8")]] RWTexture2D<float4> image : register(u0);
RWByteAddressBuffer ray_counter : register(u1);

static const float3 SUN = float3(0.42f, 0.78f, -0.46f);

bool occluded(float3 origin, float3 dir, float tmax) {
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0f;
    r.TMax = tmax;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {}
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 sky(float3 d) { return lerp(float3(0.62f, 0.72f, 0.86f), float3(0.20f, 0.36f, 0.70f), saturate(d.y * 1.5f)); }

[numthreads(8, 8, 1)]
void cs_rt_trace(uint2 id : SV_DispatchThreadID) {
    const bool inside = all(id < pc.u.xy);
    uint rays = 0;
    float3 col = 0.0f;
    if (inside) {
        const float t = pc.f.x;
        const float3 eye = float3(sin(t * 0.11f) * 30.0f, 26.0f, -70.0f + cos(t * 0.11f) * 10.0f);
        const float3 fwd = normalize(float3(0.0f, 2.0f, 0.0f) - eye);
        const float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), fwd));
        const float3 up = cross(fwd, right);
        const float2 ndc = (float2(id) + 0.5f) / float2(pc.u.xy) * 2.0f - 1.0f;
        const float tan_half = 0.6f;
        RayDesc r;
        r.Origin = eye;
        r.Direction = normalize(fwd + right * ndc.x * tan_half * pc.f.y - up * ndc.y * tan_half);
        r.TMin = 0.01f;
        r.TMax = 1000.0f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
        while (q.Proceed()) {}
        rays = 1;
        col = sky(r.Direction);
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            const float3 p = r.Origin + r.Direction * q.CommittedRayT();
            float3 n, albedo;
            const uint inst = q.CommittedInstanceID();
            if (inst == 0) {
                const uint3 tri = terrain_idx.Load3(q.CommittedPrimitiveIndex() * 12u);
                const float3 a = asfloat(terrain_vtx.Load3(tri.x * 12u));
                const float3 b = asfloat(terrain_vtx.Load3(tri.y * 12u));
                const float3 c = asfloat(terrain_vtx.Load3(tri.z * 12u));
                n = normalize(cross(b - a, c - a));
                n = n.y < 0.0f ? -n : n;
                albedo = lerp(float3(0.30f, 0.42f, 0.18f), float3(0.55f, 0.50f, 0.42f), saturate(p.y * 0.18f + 0.5f));
            } else {
                const float3 op = q.CommittedObjectRayOrigin() + q.CommittedObjectRayDirection() * q.CommittedRayT();
                n = normalize(mul(q.CommittedObjectToWorld3x4(), float4(op, 0.0f)));
                albedo = float3(0.25f + 0.7f * hash01(inst), 0.25f + 0.7f * hash01(inst * 3u + 1u), 0.25f + 0.7f * hash01(inst * 7u + 2u));
            }
            const float3 o = p + n * 0.02f;
            const float3 sun = normalize(SUN);
            const float lit = occluded(o, sun, 500.0f) ? 0.0f : saturate(dot(n, sun));
            // Two ambient-occlusion rays in the hemisphere around n (per-pixel, per-frame hash).
            const float3 tx = normalize(abs(n.y) < 0.9f ? cross(n, float3(0.0f, 1.0f, 0.0f)) : cross(n, float3(1.0f, 0.0f, 0.0f)));
            const float3 ty = cross(n, tx);
            float ao = 0.0f;
            [unroll] for (uint k = 0; k < 2; ++k) {
                const uint h = hash_u32(id.x * 7919u ^ id.y * 104729u ^ pc.u.z * 0x9e3779b9u ^ k * 0x85ebca6bu);
                const float phi = (h & 0xFFFFu) / 65536.0f * 6.2831853f, ct = sqrt(((h >> 16) + 0.5f) / 65536.0f);
                const float st = sqrt(1.0f - ct * ct);
                const float3 d = tx * (cos(phi) * st) + ty * (sin(phi) * st) + n * ct;
                ao += occluded(o, d, 4.0f) ? 0.0f : 0.5f;
            }
            rays += 3;
            col = albedo * (0.85f * lit + 0.35f * ao * sky(n));
        }
        image[id] = float4(col, 1.0f);
    }
    // Ray count: one atomic per wave.
    const uint total = WaveActiveSum(rays);
    if (WaveIsFirstLane() && total) ray_counter.InterlockedAdd(0, total);
}
