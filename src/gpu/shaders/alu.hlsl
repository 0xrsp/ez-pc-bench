// ALU throughput: 8 independent 4-wide multiply-add chains per thread.
// pc.u.x = iterations. Ops per thread per iteration = 8 chains * 4 lanes * 2 = 64.
#include "common.hlsli"

RWByteAddressBuffer out_buf : register(u0);

[numthreads(256, 1, 1)]
void cs_fp32(uint tid : SV_DispatchThreadID) {
    const float4 m = float4(0.9999f, 1.0001f, 0.9998f, 1.0002f);
    const float4 k = pc.f * 0.5f;
    float4 a0 = pc.f + (float)tid, a1 = a0 + 1.0f, a2 = a0 + 2.0f, a3 = a0 + 3.0f;
    float4 a4 = a0 + 4.0f, a5 = a0 + 5.0f, a6 = a0 + 6.0f, a7 = a0 + 7.0f;
    for (uint i = 0; i < pc.u.x; ++i) {
        a0 = mad(a0, m, k); a1 = mad(a1, m, k); a2 = mad(a2, m, k); a3 = mad(a3, m, k);
        a4 = mad(a4, m, k); a5 = mad(a5, m, k); a6 = mad(a6, m, k); a7 = mad(a7, m, k);
    }
    const float4 s = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
    // Practically never true; makes the result observable so the loop can't be removed.
    if (asuint(s.x + s.y + s.z + s.w) == 0x7f800001u) out_buf.Store(tid * 4, 1u);
}

[numthreads(256, 1, 1)]
void cs_int32(uint tid : SV_DispatchThreadID) {
    const uint4 m = uint4(1664525u, 22695477u, 1103515245u, 214013u);
    const uint4 k = pc.u.yzwy | 1u;
    uint4 a0 = tid + uint4(0, 1, 2, 3), a1 = a0 * 3u, a2 = a0 * 5u, a3 = a0 * 7u;
    uint4 a4 = a0 * 11u, a5 = a0 * 13u, a6 = a0 * 17u, a7 = a0 * 19u;
    for (uint i = 0; i < pc.u.x; ++i) {
        a0 = a0 * m + k; a1 = a1 * m + k; a2 = a2 * m + k; a3 = a3 * m + k;
        a4 = a4 * m + k; a5 = a5 * m + k; a6 = a6 * m + k; a7 = a7 * m + k;
    }
    const uint4 s = a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
    if ((s.x ^ s.y ^ s.z ^ s.w) == 0x12345678u) out_buf.Store(tid * 4, 1u);
}
