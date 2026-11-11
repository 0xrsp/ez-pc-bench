// FP16 ALU throughput (packed math). Compiled with -enable-16bit-types, SM 6.2.
// Requires D3D12 Native16BitShaderOpsSupported / Vulkan shaderFloat16; skip the test otherwise.
// pc.u.x = iterations. Ops per thread per iteration = 8 chains * 4 lanes * 2 = 64 (same as FP32).
#include "common.hlsli"

RWByteAddressBuffer out_buf : register(u0);

[numthreads(256, 1, 1)]
void cs_fp16(uint tid : SV_DispatchThreadID) {
    const float16_t4 m = float16_t4(0.999h, 1.001h, 0.998h, 1.002h);
    const float16_t4 k = (float16_t4)(pc.f * 0.001f);
    float16_t4 a0 = (float16_t4)(pc.f * 0.01f) + (float16_t)(tid & 7);
    float16_t4 a1 = a0 + 1.0h, a2 = a0 + 2.0h, a3 = a0 + 3.0h, a4 = a0 + 4.0h, a5 = a0 + 5.0h, a6 = a0 + 6.0h, a7 = a0 + 7.0h;
    for (uint i = 0; i < pc.u.x; ++i) {
        a0 = mad(a0, m, k); a1 = mad(a1, m, k); a2 = mad(a2, m, k); a3 = mad(a3, m, k);
        a4 = mad(a4, m, k); a5 = mad(a5, m, k); a6 = mad(a6, m, k); a7 = mad(a7, m, k);
    }
    const float4 s = (float4)(a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7);
    if (asuint(s.x + s.y + s.z + s.w) == 0x7f800001u) out_buf.Store(tid * 4, 1u);
}
