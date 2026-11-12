// API-independent helpers of the extra GPU tests (gpu_ext.h): working-set limits, the latency chain,
// pipeline seeds, texture data.
#include "gpu/gpu_ext.h"
#include "core/timer.h"
#include <windows.h>
#include <string.h>

u64 gpu_ext_mem_cap(const BenchParams* p) {
    const GpuInfo* g = gpu_selected(p);
    return g && g->dedicated_vram >= 4 * GiB ? 512 * MiB : 256 * MiB;
}

f64 gpu_ext_point_seconds(const BenchParams* p) {
    const f64 s = p->duration_ms / 1000.0 / 16.0;
    return s < 0.1 ? 0.1 : s;
}

void gpu_ext_progress(BenchLive* live, f64 elapsed, f64 total, f32 p0, f32 p1) {
    const f64 frac = total > 0.0 ? elapsed / total : 1.0;
    live_progress(live, p0 + (p1 - p0) * (f32)(frac < 1.0 ? frac : 1.0));
}

static u64 xorshift(u64* s) {
    u64 x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

void gpu_ext_make_chain(u32* next, u32 n) {
    // Sattolo's shuffle of the identity yields a cyclic permutation: i -> next[i] visits every element once.
    for (u32 i = 0; i < n; ++i) next[i] = i;
    u64 rng = 0x2545F4914F6CDD1Dull;
    for (u32 i = n - 1; i > 0; --i) {
        const u32 k = (u32)(xorshift(&rng) % i);
        const u32 t = next[i]; next[i] = next[k]; next[k] = t;
    }
}

u64 gpu_ext_pso_seed() {
    static u64 s_state;
    if (!s_state) s_state = (u64)timer_ticks() * 0x9E3779B97F4A7C15ull | 1;
    return xorshift(&s_state);
}

// ---- textures ------------------------------------------------------------------------------------

const char* const GE_TEX_FORMAT_NAMES[GF_COUNT] = {
    "RGBA8", "R32F", "RGBA16F", "RGBA32F", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7",
};

const char* const GE_ANISO_NAMES[GE_ANISO_MODES] = {
    "Bilinear", "Trilinear", "Anisotropic 2x", "Anisotropic 4x", "Anisotropic 8x", "Anisotropic 16x",
};

u32 gpu_ext_tex_unit_bytes(GeTexFormat f, bool* block) {
    *block = f >= GF_BC1;
    switch (f) {
    case GF_RGBA8: case GF_R32F: return 4;
    case GF_RGBA16F: return 8;
    case GF_RGBA32F: return 16;
    case GF_BC1: case GF_BC4: return 8;
    default: return 16;     // BC3, BC5, BC6H, BC7
    }
}

static u16 half_of(f32 v) {    // 0 <= v < 2, normal range only
    u32 b;
    memcpy(&b, &v, 4);
    const i32 e = (i32)((b >> 23) & 0xFF) - 127 + 15;
    if (e <= 0) return 0;
    return (u16)((e << 10) | ((b >> 13) & 0x3FF));
}

void gpu_ext_tex_fill(GeTexFormat f, u8* dst, u32 size, u32 seed) {
    bool block;
    const u32 unit = gpu_ext_tex_unit_bytes(f, &block);
    const u64 units = block ? (u64)(size / 4) * (size / 4) : (u64)size * size;
    u64 rng = 0x9E3779B97F4A7C15ull ^ ((u64)seed << 32 | seed);
    if (f == GF_R32F || f == GF_RGBA32F) {
        f32* d = (f32*)dst;
        for (u64 i = 0; i < units * unit / 4; ++i) d[i] = (f32)(xorshift(&rng) >> 40) / 16777216.0f;
    } else if (f == GF_RGBA16F) {
        u16* d = (u16*)dst;
        for (u64 i = 0; i < units * 4; ++i) d[i] = half_of((f32)(xorshift(&rng) >> 40) / 16777216.0f);
    } else {
        // RGBA8 texels and BC blocks: random bytes (decode to noise; BC6H / BC7 reserved modes decode to 0).
        u64* d = (u64*)dst;
        for (u64 i = 0; i < units * unit / 8; ++i) d[i] = xorshift(&rng);
    }
}
