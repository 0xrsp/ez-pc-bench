// VRAM bandwidth. pc.u.x = number of 16-byte elements in the buffer, pc.u.y = total thread count.
// Each thread walks elements tid, tid + threads, ... (coalesced 16-byte accesses).
#include "common.hlsli"

ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);

[numthreads(256, 1, 1)]
void cs_bw_read(uint tid : SV_DispatchThreadID) {
    uint4 acc = 0;
    for (uint i = tid; i < pc.u.x; i += pc.u.y) acc ^= src.Load4(i * 16);
    if ((acc.x ^ acc.y ^ acc.z ^ acc.w) == 0x9e3779b9u) dst.Store(tid * 4, 1u);
}

[numthreads(256, 1, 1)]
void cs_bw_write(uint tid : SV_DispatchThreadID) {
    for (uint i = tid; i < pc.u.x; i += pc.u.y) dst.Store4(i * 16, uint4(i, i ^ 0x55555555u, tid, pc.u.z));
}

[numthreads(256, 1, 1)]
void cs_bw_copy(uint tid : SV_DispatchThreadID) {
    for (uint i = tid; i < pc.u.x; i += pc.u.y) dst.Store4(i * 16, src.Load4(i * 16));
}
