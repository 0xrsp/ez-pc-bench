// Groupshared (LDS) read bandwidth. pc.u.x = iterations; 4 x 16-byte LDS reads per thread per iteration.
#include "common.hlsli"

RWByteAddressBuffer out_buf : register(u0);
groupshared float4 lds[1024];   // 16 KiB

[numthreads(256, 1, 1)]
void cs_lds(uint tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    for (uint j = gi; j < 1024; j += 256) lds[j] = float4(j, j + 1, j + 2, j + 3) * pc.f.x;
    GroupMemoryBarrierWithGroupSync();
    float4 acc = 0;
    for (uint i = 0; i < pc.u.x; ++i) {
        acc += lds[(gi + i) & 1023];
        acc += lds[(gi * 3 + i + 256) & 1023];
        acc += lds[(gi * 5 + i + 512) & 1023];
        acc += lds[(gi * 7 + i + 768) & 1023];
    }
    if (asuint(acc.x + acc.y + acc.z + acc.w) == 0x7f800001u) out_buf.Store(tid * 4, 1u);
}
