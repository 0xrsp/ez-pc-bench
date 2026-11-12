// Stability tester shaders (src/stability/stab_gpu.cpp, D3D12). Bindings follow src/gpu/gpu_common.h: 8 root
// constants at b0, UAVs u0..u2 (root descriptors here). The shared build step also emits SPIR-V (unused).
//
// Every float operation that feeds a checked result is `precise` (no mad contraction, no reordering): D3D
// requires correctly rounded add / multiply and exact floor, so the CPU references in stab_gpu.cpp match bit for
// bit. Values stay in [0, 4): no denormals, no infinities.

struct StabParams {
    uint4 a;
    uint4 b;
};
[[vk::push_constant]] ConstantBuffer<StabParams> pc : register(b0);

RWStructuredBuffer<uint> buf : register(u0);    // VRAM chunk / render copy / power buffer
RWStructuredBuffer<uint> errs : register(u1);   // error counters + log (layout below, mirrored in stab_gpu.cpp)
RWStructuredBuffer<uint> refs : register(u2);   // compute references (4 per thread)

#define ERR_TOTAL 0
#define ERR_CHUNK 16        // 64 per-chunk counters
#define ERR_LOG 128         // ERR_LOG_MAX entries of 4: chunk, index, expected, actual
#define ERR_LOG_MAX 16

void record(uint chunk, uint index, uint expected, uint actual) {
    uint old;
    InterlockedAdd(errs[ERR_TOTAL], 1u, old);
    InterlockedAdd(errs[ERR_CHUNK + (chunk & 63u)], 1u);
    if (old < ERR_LOG_MAX) {
        const uint o = ERR_LOG + old * 4u;
        errs[o] = chunk;
        errs[o + 1] = index;
        errs[o + 2] = expected;
        errs[o + 3] = actual;
    }
}

uint hash_u32(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ---- VRAM pattern test --------------------------------------------------------------------------------
// pc.a = (elements, pattern kind, seed, chunk), pc.b = (xor mask, inject index or ~0, rewrite with complement, stride)

uint vram_pattern(uint kind, uint i, uint seed, uint chunk) {
    if (kind == 0) return i ^ (chunk << 27);                // address in address (word index + chunk id)
    if (kind == 1) return 1u << ((i + seed) & 31u);         // walking 1
    if (kind == 2) return ~(1u << ((i + seed) & 31u));      // walking 0
    if (kind == 3) return hash_u32(i ^ (seed * 0x9E3779B9u));  // random
    return seed;                                            // solid (moving inversions)
}

[numthreads(256, 1, 1)]
void cs_stab_fill(uint tid : SV_DispatchThreadID) {
    for (uint i = tid; i < pc.a.x; i += pc.b.w) {
        uint v = vram_pattern(pc.a.y, i, pc.a.z, pc.a.w) ^ pc.b.x;
        if (i == pc.b.y) v ^= 0x100u;      // dev self-test: one wrong bit
        buf[i] = v;
    }
}

[numthreads(256, 1, 1)]
void cs_stab_check(uint tid : SV_DispatchThreadID) {
    for (uint i = tid; i < pc.a.x; i += pc.b.w) {
        const uint e = vram_pattern(pc.a.y, i, pc.a.z, pc.a.w) ^ pc.b.x;
        const uint v = buf[i];
        if (v != e) record(pc.a.w, i, e, v);
        if (pc.b.z != 0) buf[i] = ~e;
    }
}

// ---- known-answer compute + render ---------------------------------------------------------------------

static const float4 MUL = asfloat(uint4(0x406CCCCDu, 0x40533333u, 0x4039999Au, 0x4079999Au));   // 3.7 3.3 2.9 3.9
static const float4 ADD = asfloat(uint4(0x3DCCCCCDu, 0x3E4CCCCDu, 0x3E99999Au, 0x3E19999Au));   // 0.1 0.2 0.3 0.15

// pc.a = (iterations, seed, inject thread or ~0, -)
[numthreads(256, 1, 1)]
void cs_stab_compute(uint tid : SV_DispatchThreadID) {
    uint4 a, b;
    precise float4 x;
    [unroll] for (uint k = 0; k < 4; ++k) {
        a[k] = hash_u32(tid * 4u + k + pc.a.y * 0x9E3779B9u);
        b[k] = hash_u32(a[k] ^ 0x85EBCA6Bu);
        x[k] = (float)(b[k] & 0xFFFFFFu) * (1.0f / 16777216.0f);
    }
    for (uint i = 0; i < pc.a.x; ++i) {
        a = a * 1664525u + (b ^ (a >> 13));
        b = ((b << 7) | (b >> 25)) ^ a;
        precise float4 t = x * MUL;
        precise float4 u = t + ADD;
        x = u - floor(u);
    }
    uint4 r = a ^ (b << 1) ^ asuint(x);
    if (tid == pc.a.z) r.y ^= 0x10u;       // dev self-test
    [unroll] for (uint j = 0; j < 4; ++j) {
        const uint e = refs[tid * 4u + j];
        if (r[j] != e) record(0, tid * 4u + j, e, r[j]);
    }
}

uint render_value(uint px, uint py, uint seed, uint rounds) {
    uint a = hash_u32((px * 0x8DA6B343u) ^ (py * 0xD8163841u) ^ seed);
    uint b = hash_u32(a ^ 0x85EBCA6Bu);
    precise float f = (float)(b & 0xFFFFFFu) * (1.0f / 16777216.0f);
    for (uint i = 0; i < rounds; ++i) {
        a = a * 1664525u + (b ^ (a >> 13));
        b = ((b << 7) | (b >> 25)) ^ a;
        precise float t = f * MUL.x;
        precise float u = t + ADD.x;
        f = u - floor(u);
    }
    return a ^ (b << 1) ^ asuint(f);
}

// Full-screen triangle, pc.b.x instances drawn over each other (the last one must win: D3D keeps primitive order).
void vs_stab_full(uint vid : SV_VertexID, uint iid : SV_InstanceID, out float4 pos : SV_Position,
                  out nointerpolation uint inst : INST) {
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    inst = iid;
}

// pc.a = (rounds, seed, inject, width), pc.b.x = instances
uint ps_stab_render(float4 pos : SV_Position, nointerpolation uint inst : INST) : SV_Target {
    const uint px = (uint)pos.x, py = (uint)pos.y;
    uint v = render_value(px, py, pc.a.y + inst, pc.a.x);
    if (pc.a.z != 0 && px == 7 && py == 7 && inst + 1 == pc.b.x) v ^= 0x40000u;    // dev self-test
    return v;
}

// One thread per pixel of the copied render target (tightly packed rows of pc.a.w).
[numthreads(256, 1, 1)]
void cs_stab_render_check(uint tid : SV_DispatchThreadID) {
    const uint w = pc.a.w;
    if (tid >= w * w) return;
    const uint e = render_value(tid % w, tid / w, pc.a.y + pc.b.x - 1u, pc.a.x);
    const uint v = buf[tid];
    if (v != e) record(1, tid, e, v);
}

// ---- maximum power: VRAM streaming + dense FMA, unchecked ----------------------------------------------
// pc.a = (uint4 elements, FMA iterations, stride, -)
[numthreads(256, 1, 1)]
void cs_stab_power(uint tid : SV_DispatchThreadID) {
    const float4 m = float4(0.9999f, 1.0001f, 0.9998f, 1.0002f);
    for (uint i = tid; i < pc.a.x; i += pc.a.z) {
        float4 v = asfloat(uint4(buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3])) * 1e-3f + 0.5f;
        float4 w0 = v, w1 = v + 1.0f, w2 = v + 2.0f, w3 = v + 3.0f;
        for (uint k = 0; k < pc.a.y; ++k) {
            w0 = mad(w0, m, 0.0001f); w1 = mad(w1, m, 0.0002f); w2 = mad(w2, m, 0.0003f); w3 = mad(w3, m, 0.0004f);
        }
        const float4 s = w0 + w1 + w2 + w3;
        buf[i * 4] = asuint(s.x); buf[i * 4 + 1] = asuint(s.y); buf[i * 4 + 2] = asuint(s.z); buf[i * 4 + 3] = asuint(s.w);
    }
}
