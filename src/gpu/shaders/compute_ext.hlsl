// Extra compute throughput tests: FP64, INT8 dot product (dp4a) and atomics. Workloads: gpu_ext.h.
#include "common.hlsli"

RWByteAddressBuffer out_buf : register(u0);

// FP64: same shape as cs_fp32 (8 independent 4-wide multiply-add chains, 64 flops per thread per iteration).
// Conversions go through float: int <-> double conversions are an optional D3D12 double extension.
[numthreads(256, 1, 1)]
void cs_fp64(uint tid : SV_DispatchThreadID) {
    const double4 m = double4(0.9999, 1.0001, 0.9998, 1.0002);
    const double4 k = (double4)pc.f * 0.5;
    double4 a0 = (double4)(pc.f + (float)tid), a1 = a0 + 1.0, a2 = a0 + 2.0, a3 = a0 + 3.0;
    double4 a4 = a0 + 4.0, a5 = a0 + 5.0, a6 = a0 + 6.0, a7 = a0 + 7.0;
    for (uint i = 0; i < pc.u.x; ++i) {
        a0 = mad(a0, m, k); a1 = mad(a1, m, k); a2 = mad(a2, m, k); a3 = mad(a3, m, k);
        a4 = mad(a4, m, k); a5 = mad(a5, m, k); a6 = mad(a6, m, k); a7 = mad(a7, m, k);
    }
    const float4 s = (float4)(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7);
    if (asuint(s.x + s.y + s.z + s.w) == 0x7f800001u) out_buf.Store(tid * 4, 1u);
}

// INT8 dot product (SM 6.4 dot4add_i8packed): 8 independent chains, each acc = dot(bytes(acc), bytes(b)) + acc,
// so the product can't be hoisted out of the loop. 8 ops per dot4add (4 multiplies + 4 adds), 64 per iteration.
[numthreads(256, 1, 1)]
void cs_dp4a(uint tid : SV_DispatchThreadID) {
    const uint b = pc.u.y | 0x01010101u;
    int a0 = (int)hash_u32(tid), a1 = a0 ^ 0x11, a2 = a0 ^ 0x22, a3 = a0 ^ 0x33;
    int a4 = a0 ^ 0x44, a5 = a0 ^ 0x55, a6 = a0 ^ 0x66, a7 = a0 ^ 0x77;
    for (uint i = 0; i < pc.u.x; ++i) {
        a0 = dot4add_i8packed((uint)a0, b, a0); a1 = dot4add_i8packed((uint)a1, b, a1);
        a2 = dot4add_i8packed((uint)a2, b, a2); a3 = dot4add_i8packed((uint)a3, b, a3);
        a4 = dot4add_i8packed((uint)a4, b, a4); a5 = dot4add_i8packed((uint)a5, b, a5);
        a6 = dot4add_i8packed((uint)a6, b, a6); a7 = dot4add_i8packed((uint)a7, b, a7);
    }
    if ((a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7) == 0x12345678) out_buf.Store(tid * 4, 1u);
}

// Atomics, pc.u.x = atomic adds per thread.
// One address: every thread of the dispatch adds to the same word (fully contended).
[numthreads(256, 1, 1)]
void cs_atomic_one(uint tid : SV_DispatchThreadID) {
    for (uint i = 0; i < pc.u.x; ++i) out_buf.InterlockedAdd(0, 1u);
}

// Unique addresses: each thread adds to its own word (no contention, neighbouring threads share cache lines).
[numthreads(256, 1, 1)]
void cs_atomic_unique(uint tid : SV_DispatchThreadID) {
    for (uint i = 0; i < pc.u.x; ++i) out_buf.InterlockedAdd(tid * 4, 1u);
}

// Shared memory (LDS): each lane adds to a different word of 256, rotating every iteration (no bank conflicts).
groupshared uint g_lds[256];

[numthreads(256, 1, 1)]
void cs_atomic_lds(uint tid : SV_DispatchThreadID, uint li : SV_GroupIndex) {
    g_lds[li] = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint i = 0; i < pc.u.x; ++i) InterlockedAdd(g_lds[(li + i) & 255], 1u);
    GroupMemoryBarrierWithGroupSync();
    if (g_lds[li] == 0x12345678u) out_buf.Store(tid * 4, 1u);
}
