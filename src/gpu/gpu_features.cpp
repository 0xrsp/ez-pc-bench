// Feature tests: procedural scenes and statistics shared by both backends. Contract: gpu_features.h.
#include "gpu/gpu_features.h"
#include <algorithm>
#include <math.h>
#include <string.h>

static u32 hash_u32(u32 x) {    // = common.hlsli
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static f32 hash01(u32 x) { return (hash_u32(x) & 0xFFFFFFu) / 16777216.0f; }

// ---- ray tracing scene ----------------------------------------------------------------------------------

void feat_terrain_indices(u32* idx) {
    for (u32 z = 0; z < FR_GRID; ++z)
        for (u32 x = 0; x < FR_GRID; ++x) {
            const u32 a = z * FR_GRID_VERTS + x, b = a + 1, c = a + FR_GRID_VERTS, d = c + 1;
            u32* t = idx + (z * FR_GRID + x) * 6;
            t[0] = a; t[1] = c; t[2] = b;
            t[3] = b; t[4] = c; t[5] = d;
        }
}

void feat_sphere_mesh(f32* xyz, u32* idx) {
    for (u32 r = 0; r <= FR_SPHERE_RINGS; ++r)
        for (u32 s = 0; s <= FR_SPHERE_SEG; ++s) {
            const f32 theta = 3.14159265f * r / FR_SPHERE_RINGS, phi = 6.28318531f * s / FR_SPHERE_SEG;
            f32* v = xyz + (r * (FR_SPHERE_SEG + 1) + s) * 3;
            v[0] = sinf(theta) * cosf(phi);
            v[1] = cosf(theta);
            v[2] = sinf(theta) * sinf(phi);
        }
    for (u32 r = 0; r < FR_SPHERE_RINGS; ++r)
        for (u32 s = 0; s < FR_SPHERE_SEG; ++s) {
            const u32 a = r * (FR_SPHERE_SEG + 1) + s, b = a + 1, c = a + FR_SPHERE_SEG + 1, d = c + 1;
            u32* t = idx + (r * FR_SPHERE_SEG + s) * 6;
            t[0] = a; t[1] = b; t[2] = c;
            t[3] = b; t[4] = d; t[5] = c;
        }
}

void feat_rt_instances(FeatInstance* inst, f32 time, u64 terrain_blas, u64 sphere_blas) {
    const u32 flags = 0x1u | 0x4u;      // triangle cull disable | force opaque (same bits in both APIs)
    memset(inst, 0, sizeof(FeatInstance) * FR_INSTANCES);
    inst[0].transform[0] = inst[0].transform[5] = inst[0].transform[10] = 1.0f;
    inst[0].id_mask = 0xFFu << 24;
    inst[0].offset_flags = flags << 24;
    inst[0].blas = terrain_blas;
    // Spheres on a jittered 64 x 32 grid over the terrain, bobbing above it.
    const f32 extent = FR_TERRAIN_SIZE * 0.45f;
    for (u32 i = 0; i < FR_SPHERES; ++i) {
        FeatInstance* t = &inst[i + 1];
        const f32 h0 = hash01(i * 4 + 0), h1 = hash01(i * 4 + 1), h2 = hash01(i * 4 + 2), h3 = hash01(i * 4 + 3);
        const f32 x = ((i % 64) + 0.2f + 0.6f * h0) / 64.0f * 2.0f * extent - extent;
        const f32 z = ((i / 64) + 0.2f + 0.6f * h1) / 32.0f * 2.0f * extent - extent;
        const f32 r = 0.8f + 1.4f * h2;
        const f32 y = 7.0f + 4.0f * h3 + 2.5f * sinf(time * (0.6f + h2) + h0 * 6.28f);
        t->transform[0] = t->transform[5] = t->transform[10] = r;
        t->transform[3] = x;
        t->transform[7] = y;
        t->transform[11] = z;
        t->id_mask = (i + 1) | 0xFFu << 24;
        t->offset_flags = flags << 24;
        t->blas = sphere_blas;
    }
}

// ---- mesh shader scene -----------------------------------------------------------------------------------

void feat_mesh_indices(u16* idx) {
    for (u32 m = 0; m < FM_MESHLETS; ++m)
        for (u32 t = 0; t < FM_MESHLET_TRIS; ++t) {     // same triangles as ms_mesh
            const u32 q = t >> 1, a = (q / 8) * 9 + q % 8, base = m * FM_MESHLET_VERTS;
            u16* o = idx + (m * FM_MESHLET_TRIS + t) * 3;
            if (t & 1) {
                o[0] = (u16)(base + a + 1); o[1] = (u16)(base + a + 10); o[2] = (u16)(base + a + 9);
            } else {
                o[0] = (u16)(base + a); o[1] = (u16)(base + a + 1); o[2] = (u16)(base + a + 9);
            }
        }
}

// ---- VRS rate image --------------------------------------------------------------------------------------

f64 feat_vrs_rate_image(u8* rates, u32 w, u32 h, u32 tile) {
    const u32 tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
    u64 reduced = 0;
    for (u32 y = 0; y < th; ++y)
        for (u32 x = 0; x < tw; ++x) {
            const f32 dx = ((x + 0.5f) * tile - w * 0.5f) / (h * 0.5f), dy = ((y + 0.5f) * tile - h * 0.5f) / (h * 0.5f);
            const f32 dist = sqrtf(dx * dx + dy * dy);
            const u8 r = dist < 0.45f ? FV_RATE_1X1 : dist < 0.85f ? FV_RATE_2X2 : FV_RATE_4X4;
            rates[y * tw + x] = r;
            if (r != FV_RATE_1X1) {
                const u32 pw = std::min(tile, w - x * tile), ph = std::min(tile, h - y * tile);
                reduced += (u64)pw * ph;
            }
        }
    return (f64)reduced / ((f64)w * h);
}

// ---- sampler feedback texture ----------------------------------------------------------------------------

u64 feat_sfb_texture_bytes() {
    u64 total = 0;
    for (u32 l = 0; l < FS_MIPS; ++l) total += (u64)(FS_TEX >> l) * (FS_TEX >> l) * 4;
    return total;
}

void feat_sfb_texture(u32* texels) {
    // Level 0: colored 32-texel tiles with per-texel grain and dark tile borders.
    for (u32 y = 0; y < FS_TEX; ++y)
        for (u32 x = 0; x < FS_TEX; ++x) {
            const u32 tile = hash_u32((y / 32) * 977u + x / 32), grain = hash_u32(y * FS_TEX + x) & 0x1F;
            const bool border = (x % 32) < 2 || (y % 32) < 2;
            u32 c = 0;
            for (u32 k = 0; k < 3; ++k) {
                u32 ch = 60 + ((tile >> (k * 8)) & 0x7F) + grain;
                if (border) ch /= 4;
                c |= (ch > 255 ? 255 : ch) << (k * 8);
            }
            texels[y * FS_TEX + x] = c | 0xFF000000u;
        }
    // Mips: 2 x 2 box filter.
    u32* src = texels;
    for (u32 l = 1; l < FS_MIPS; ++l) {
        const u32 sw = FS_TEX >> (l - 1), dw = sw / 2;
        u32* dst = src + (u64)sw * sw;
        for (u32 y = 0; y < dw; ++y)
            for (u32 x = 0; x < dw; ++x) {
                const u32 s[4] = { src[(2 * y) * sw + 2 * x], src[(2 * y) * sw + 2 * x + 1], src[(2 * y + 1) * sw + 2 * x],
                                   src[(2 * y + 1) * sw + 2 * x + 1] };
                u32 c = 0;
                for (u32 k = 0; k < 4; ++k) {
                    u32 sum = 0;
                    for (u32 i = 0; i < 4; ++i) sum += (s[i] >> (k * 8)) & 0xFF;
                    c |= ((sum + 2) / 4) << (k * 8);
                }
                dst[y * dw + x] = c;
            }
        src = dst;
    }
}

// ---- pass statistics -----------------------------------------------------------------------------------------

constexpr u32 MAX_PASS_FRAMES = 1u << 18;
static f32 s_pass[MAX_PASS_FRAMES];
static u32 s_pass_count;

void feat_pass_begin() { s_pass_count = 0; }

void feat_pass_push(f32 frame_ms) {
    if (s_pass_count < MAX_PASS_FRAMES) s_pass[s_pass_count++] = frame_ms;
}

FeatPass feat_pass_end() {
    FeatPass r = {};
    const u32 n = s_pass_count;
    r.frames = n;
    if (n == 0) return r;
    f64 total = 0.0;
    for (u32 i = 0; i < n; ++i) total += s_pass[i];
    std::sort(s_pass, s_pass + n);
    r.avg_ms = total / n;
    r.p99_ms = s_pass[(u32)(0.99 * (n - 1) + 0.5)];
    r.p999_ms = s_pass[(u32)(0.999 * (n - 1) + 0.5)];
    return r;
}

f64 feat_gain_pct(f64 base_ms, f64 feature_ms) { return feature_ms > 0.0 ? (base_ms / feature_ms - 1.0) * 100.0 : 0.0; }

f64 feat_pass_warmup(const BenchParams* p, u32 pass) {
    const f64 w = gpu_warmup_seconds(p);
    return pass == 0 ? w : (w < 0.25 ? w : 0.25);
}
