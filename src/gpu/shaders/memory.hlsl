// GPU memory hierarchy: pointer-chase latency and read bandwidth vs working-set size. Workloads: gpu_ext.h.
#include "common.hlsli"

ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);

// One thread follows the chain; every element holds the byte offset of the next one. Each dispatch continues
// where the previous one stopped (position kept in dst[0], masked into the current working set), so the walk
// covers the whole set instead of re-reading the first steps * stride bytes.
// pc.u.x = steps (multiple of 4), pc.u.z = (working set - 1) & ~(stride - 1).
[numthreads(1, 1, 1)]
void cs_chase(uint tid : SV_DispatchThreadID) {
    uint p = dst.Load(0) & pc.u.z;
    for (uint i = 0; i < pc.u.x; i += 4) {
        p = src.Load(p);
        p = src.Load(p);
        p = src.Load(p);
        p = src.Load(p);
    }
    dst.Store(0, p);
}

// Builds the chain from the compact successor list in t0: element i (at i * stride) = next[i] * stride.
// pc.u.x = element count, pc.u.y = stride in bytes.
[numthreads(256, 1, 1)]
void cs_chase_init(uint tid : SV_DispatchThreadID) {
    if (tid < pc.u.x) dst.Store(tid * pc.u.y, src.Load(tid * 4) * pc.u.y);
}

// Read bandwidth: every group reads pc.u.y x 16 bytes per thread (coalesced uint4 loads) from a window that
// wraps inside the working set, so with a small working set every compute unit re-reads the whole set from
// its own cache, and larger sets are covered by the groups' consecutive windows (2 GiB per dispatch).
// pc.u.x = working set / 16 - 1 (power of two), pc.u.y = loads per thread, pc.u.z = start element.
[numthreads(256, 1, 1)]
void cs_sweep(uint3 gid : SV_GroupID, uint li : SV_GroupIndex) {
    uint idx = pc.u.z + gid.x * 256 * pc.u.y + li;
    uint4 acc = 0;
    for (uint i = 0; i < pc.u.y; ++i) {
        acc ^= src.Load4((idx & pc.u.x) * 16);
        idx += 256;
    }
    if ((acc.x ^ acc.y ^ acc.z ^ acc.w) == 0x9e3779b9u) dst.Store(li * 4, 1u);
}

// Fills dst with non-trivial data. pc.u.x = 16-byte elements, pc.u.y = total thread count.
[numthreads(256, 1, 1)]
void cs_mem_fill(uint tid : SV_DispatchThreadID) {
    for (uint i = tid; i < pc.u.x; i += pc.u.y) dst.Store4(i * 16, uint4(hash_u32(i), i, ~i, i * 0x9e3779b9u));
}
