// Scientific / graphics kernels in plain C (the compiler vectorizes them as it would real code):
// complex FP64 FFT (radix-4 Stockham with a radix-2 step), O(n^2) n-body gravity (FP32), and ray-box /
// ray-triangle intersection. Each self-checks against a reference or a conservation law.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <immintrin.h>
#include <math.h>
#include <string.h>

static u64 rng(u64* s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static f64 rng_unit(u64* s) { return (f64)(rng(s) >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)

// ---- FFT -----------------------------------------------------------------------------------------------

// Cpx, fft_stockham and fft_twiddles are shared with the pi workload (cpu_internal.h).
static inline Cpx cadd(Cpx a, Cpx b) { return { a.re + b.re, a.im + b.im }; }
static inline Cpx csub(Cpx a, Cpx b) { return { a.re - b.re, a.im - b.im }; }
static inline Cpx cmul(Cpx a, Cpx b) { return { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; }
static inline Cpx cmul_j(Cpx a) { return { -a.im, a.re }; }    // a * i

// 16K points: data, scratch and twiddles (768 KiB) stay in a Zen 4 L2, so MT scales with cores. At 64K
// points 16 threads were DRAM-bound (MT / ST 2.7x).
static constexpr u32 FFT_LOG2 = 14;
static constexpr u32 FFT_N = 1u << FFT_LOG2;      // 16384 points = 256 KiB per buffer

// Forward transform of x (n points, natural order in and out) using y as scratch. w[k] = exp(-2 pi i k / n).
// Returns the buffer that holds the result (x or y).
Cpx* fft_stockham(Cpx* x, Cpx* y, const Cpx* w, u32 n, u32 wstride) {
    u32 m = n, s = 1;
    while (m >= 4) {
        const u32 q4 = m / 4, wstep = n / m * wstride;
        for (u32 p = 0; p < q4; ++p) {
            const Cpx w1 = w[p * wstep], w2 = w[2 * p * wstep], w3 = w[3 * p * wstep];
            const Cpx* xa = x + (u64)s * p;
            Cpx* ya = y + (u64)s * 4 * p;
            for (u32 q = 0; q < s; ++q) {
                const Cpx a = xa[q], b = xa[q + (u64)s * q4], c = xa[q + (u64)s * 2 * q4], d = xa[q + (u64)s * 3 * q4];
                const Cpx apc = cadd(a, c), amc = csub(a, c), bpd = cadd(b, d), jbmd = cmul_j(csub(b, d));
                ya[q] = cadd(apc, bpd);
                ya[q + s] = cmul(w1, csub(amc, jbmd));
                ya[q + 2 * (u64)s] = cmul(w2, csub(apc, bpd));
                ya[q + 3 * (u64)s] = cmul(w3, cadd(amc, jbmd));
            }
        }
        m /= 4;
        s *= 4;
        Cpx* t = x; x = y; y = t;
    }
    if (m == 2) {
        for (u32 q = 0; q < s; ++q) {
            const Cpx a = x[q], b = x[q + s];
            y[q] = cadd(a, b);
            y[q + s] = csub(a, b);
        }
        Cpx* t = x; x = y; y = t;
    }
    return x;
}

// Inverse via conjugation: ifft(x) = conj(fft(conj(x))) / n.
static Cpx* fft_inverse(Cpx* x, Cpx* y, const Cpx* w, u32 n) {
    for (u32 i = 0; i < n; ++i) x[i].im = -x[i].im;
    Cpx* r = fft_stockham(x, y, w, n, 1);
    const f64 inv = 1.0 / n;
    for (u32 i = 0; i < n; ++i) { r[i].re *= inv; r[i].im = -r[i].im * inv; }
    return r;
}

struct FftCtx {
    Cpx* w;
    Cpx* buf;           // per thread: 2 buffers of FFT_N
    bool failed;
};

void fft_twiddles(Cpx* w, u32 n) {
    for (u32 k = 0; k < n; ++k) {
        const f64 a = -2.0 * 3.14159265358979323846 * k / n;
        w[k] = { cos(a), sin(a) };
    }
}

static f64 kern_fft(void* ctx, u32 index, u32) {
    FftCtx* c = (FftCtx*)ctx;
    Cpx* a = c->buf + (u64)index * 2 * FFT_N;
    Cpx* b = a + FFT_N;
    Cpx* r = fft_stockham(a, b, c->w, FFT_N, 1);       // forward then inverse keeps the data bounded
    r = fft_inverse(r, r == a ? b : a, c->w, FFT_N);
    if (r != a) memcpy(a, r, sizeof(Cpx) * FFT_N);
    return 2.0 * 5.0 * FFT_N * FFT_LOG2;               // conventional 5 n log2 n flops per transform
}

// One tone at bin 5 must give n at bin 5 and ~0 elsewhere; random data must survive a round trip.
static bool fft_selftest(FftCtx* c) {
    Cpx* a = c->buf;
    Cpx* b = a + FFT_N;
    for (u32 k = 0; k < FFT_N; ++k) {
        const f64 t = 2.0 * 3.14159265358979323846 * 5.0 * k / FFT_N;
        a[k] = { cos(t), sin(t) };
    }
    Cpx* r = fft_stockham(a, b, c->w, FFT_N, 1);
    for (u32 k = 0; k < FFT_N; ++k) {
        const f64 expect = k == 5 ? (f64)FFT_N : 0.0;
        if (fabs(r[k].re - expect) > 1e-6 || fabs(r[k].im) > 1e-6) return false;
    }
    u64 s = 42;
    for (u32 k = 0; k < FFT_N; ++k) a[k] = { rng_unit(&s) - 0.5, rng_unit(&s) - 0.5 };
    r = fft_stockham(a, b, c->w, FFT_N, 1);
    r = fft_inverse(r, r == a ? b : a, c->w, FFT_N);
    s = 42;
    for (u32 k = 0; k < FFT_N; ++k) {
        const f64 re = rng_unit(&s) - 0.5, im = rng_unit(&s) - 0.5;
        if (fabs(r[k].re - re) > 1e-12 || fabs(r[k].im - im) > 1e-12) return false;
    }
    return true;
}

void cpu_fft(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u32 mt = thread_count_for(p->thread_count);
    FftCtx c = {};
    c.w = (Cpx*)big_alloc(sizeof(Cpx) * FFT_N);
    c.buf = (Cpx*)big_alloc(sizeof(Cpx) * 2 * FFT_N * mt);
    if (!c.w || !c.buf) { big_free(c.w); big_free(c.buf); res_fail(out, "out of memory"); return; }
    fft_twiddles(c.w, FFT_N);
    const bool ok = fft_selftest(&c);
    u64 s = 7;
    for (u64 i = 0; i < (u64)FFT_N * 2 * mt; ++i) c.buf[i] = { rng_unit(&s) - 0.5, rng_unit(&s) - 0.5 };
    st_mt_test(kern_fft, &c, "GFLOPS", 1e9, p, live, out, cancel);
    big_free(c.w);
    big_free(c.buf);
    if (!ok) res_fail(out, "FFT self-test failed");
}

// ---- n-body ------------------------------------------------------------------------------------------
// Each thread simulates its own system of NB bodies: all-pairs accelerations with softening, then a
// leapfrog step. Work unit: body-body interactions.

static constexpr u32 NB = 2048;
static constexpr f32 NB_SOFT2 = 1e-3f, NB_DT = 1e-4f;

struct alignas(64) NBody {
    f32 x[NB], y[NB], z[NB], m[NB];
    f32 vx[NB], vy[NB], vz[NB];
    f32 ax[NB], ay[NB], az[NB];
};

static void nbody_init(NBody* b, u64 seed) {
    for (u32 i = 0; i < NB; ++i) {
        b->x[i] = (f32)rng_unit(&seed) * 2.0f - 1.0f;
        b->y[i] = (f32)rng_unit(&seed) * 2.0f - 1.0f;
        b->z[i] = (f32)rng_unit(&seed) * 2.0f - 1.0f;
        b->m[i] = 0.5f + (f32)rng_unit(&seed);
        b->vx[i] = b->vy[i] = b->vz[i] = 0.0f;
    }
}

static inline f32 hsum8(__m256 v) {
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_movehdup_ps(s));
    return _mm_cvtss_f32(s);
}

// AVX2, 8 bodies j at a time (precise FP keeps the compiler from vectorizing the sums itself). Exact
// sqrt + divide, not the rsqrt approximation.
static void nbody_accel(NBody* b) {
    const __m256 soft = _mm256_set1_ps(NB_SOFT2), one = _mm256_set1_ps(1.0f);
    for (u32 i = 0; i < NB; ++i) {
        const __m256 xi = _mm256_set1_ps(b->x[i]), yi = _mm256_set1_ps(b->y[i]), zi = _mm256_set1_ps(b->z[i]);
        __m256 ax = _mm256_setzero_ps(), ay = _mm256_setzero_ps(), az = _mm256_setzero_ps();
        for (u32 j = 0; j < NB; j += 8) {
            const __m256 dx = _mm256_sub_ps(_mm256_load_ps(b->x + j), xi);
            const __m256 dy = _mm256_sub_ps(_mm256_load_ps(b->y + j), yi);
            const __m256 dz = _mm256_sub_ps(_mm256_load_ps(b->z + j), zi);
            const __m256 d2 = _mm256_fmadd_ps(dx, dx, _mm256_fmadd_ps(dy, dy, _mm256_fmadd_ps(dz, dz, soft)));
            const __m256 inv = _mm256_div_ps(one, _mm256_sqrt_ps(d2));
            const __m256 s = _mm256_mul_ps(_mm256_load_ps(b->m + j), _mm256_mul_ps(inv, _mm256_mul_ps(inv, inv)));
            ax = _mm256_fmadd_ps(dx, s, ax);
            ay = _mm256_fmadd_ps(dy, s, ay);
            az = _mm256_fmadd_ps(dz, s, az);
        }
        b->ax[i] = hsum8(ax);
        b->ay[i] = hsum8(ay);
        b->az[i] = hsum8(az);
    }
}

static void nbody_step(NBody* b) {
    nbody_accel(b);
    for (u32 i = 0; i < NB; ++i) {
        b->vx[i] += b->ax[i] * NB_DT; b->vy[i] += b->ay[i] * NB_DT; b->vz[i] += b->az[i] * NB_DT;
        b->x[i] += b->vx[i] * NB_DT;  b->y[i] += b->vy[i] * NB_DT;  b->z[i] += b->vz[i] * NB_DT;
    }
}

// Pairwise forces cancel: total m*a must be ~0 compared with the sum of |m*a|.
static bool nbody_check(const NBody* b) {
    f64 sx = 0.0, sy = 0.0, sz = 0.0, mag = 0.0;
    for (u32 i = 0; i < NB; ++i) {
        const f64 fx = (f64)b->m[i] * b->ax[i], fy = (f64)b->m[i] * b->ay[i], fz = (f64)b->m[i] * b->az[i];
        sx += fx; sy += fy; sz += fz;
        mag += sqrt(fx * fx + fy * fy + fz * fz);
        if (!isfinite(fx) || !isfinite(b->x[i])) return false;
    }
    return mag > 0.0 && sqrt(sx * sx + sy * sy + sz * sz) < 1e-4 * mag;
}

struct NBodyCtx { NBody* sys; u32 steps[MAX_LP]; };

static f64 kern_nbody(void* ctx, u32 index, u32) {
    NBodyCtx* c = (NBodyCtx*)ctx;
    NBody* b = &c->sys[index];
    if (++c->steps[index] % 2000 == 0) nbody_init(b, 1234 + index);     // keep the systems from collapsing
    nbody_step(b);
    return (f64)NB * NB;
}

void cpu_nbody(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u32 mt = thread_count_for(p->thread_count);
    static NBodyCtx c;
    c.sys = (NBody*)big_alloc(sizeof(NBody) * mt);
    if (!c.sys) { res_fail(out, "out of memory"); return; }
    memset(c.steps, 0, sizeof(c.steps));
    for (u32 i = 0; i < mt; ++i) nbody_init(&c.sys[i], 1234 + i);
    st_mt_test(kern_nbody, &c, "Ginteractions/s", 1e9, p, live, out, cancel);
    nbody_accel(&c.sys[0]);
    const bool ok = nbody_check(&c.sys[0]);
    big_free(c.sys);
    if (!ok) res_fail(out, "forces don't balance (momentum not conserved)");
}

// ---- ray intersection ----------------------------------------------------------------------------------

static constexpr u32 RAY_N = 1024;          // rays per call
static constexpr u32 BOX_N = 4096;          // boxes, SoA
static constexpr u32 TRI_N = 1024;          // triangles

struct RayScene {
    f32 ox[RAY_N], oy[RAY_N], oz[RAY_N], dx[RAY_N], dy[RAY_N], dz[RAY_N];
    f32 bminx[BOX_N], bminy[BOX_N], bminz[BOX_N], bmaxx[BOX_N], bmaxy[BOX_N], bmaxz[BOX_N];
    f32 v0[TRI_N][3], e1[TRI_N][3], e2[TRI_N][3];      // vertex 0 and two edges
    u64 expect_box_hits, expect_tri_hits;              // double-precision reference over the first REF rays
};
static constexpr u32 RAY_REF = 64;

static void ray_scene_init(RayScene* s) {
    u64 r = 99;
    for (u32 i = 0; i < RAY_N; ++i) {           // rays from around the origin in random directions
        s->ox[i] = (f32)(rng_unit(&r) - 0.5); s->oy[i] = (f32)(rng_unit(&r) - 0.5); s->oz[i] = (f32)(rng_unit(&r) - 0.5);
        f64 d[3];
        f64 len;
        do {
            for (u32 k = 0; k < 3; ++k) d[k] = rng_unit(&r) * 2.0 - 1.0;
            len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        } while (len < 0.1 || len > 1.0);
        s->dx[i] = (f32)(d[0] / len); s->dy[i] = (f32)(d[1] / len); s->dz[i] = (f32)(d[2] / len);
    }
    for (u32 i = 0; i < BOX_N; ++i) {           // boxes scattered in a cube of side 20
        const f32 cx = (f32)(rng_unit(&r) * 20.0 - 10.0), cy = (f32)(rng_unit(&r) * 20.0 - 10.0), cz = (f32)(rng_unit(&r) * 20.0 - 10.0);
        const f32 h = (f32)(0.1 + rng_unit(&r) * 0.6);
        s->bminx[i] = cx - h; s->bminy[i] = cy - h; s->bminz[i] = cz - h;
        s->bmaxx[i] = cx + h; s->bmaxy[i] = cy + h; s->bmaxz[i] = cz + h;
    }
    for (u32 i = 0; i < TRI_N; ++i) {           // triangles of size ~2 on a sphere of radius ~5
        f32 c[3];
        for (u32 k = 0; k < 3; ++k) c[k] = (f32)(rng_unit(&r) * 10.0 - 5.0);
        for (u32 k = 0; k < 3; ++k) {
            s->v0[i][k] = c[k];
            s->e1[i][k] = (f32)(rng_unit(&r) * 2.0 - 1.0) * 1.5f;
            s->e2[i][k] = (f32)(rng_unit(&r) * 2.0 - 1.0) * 1.5f;
        }
    }
    // Reference counts in double precision.
    s->expect_box_hits = s->expect_tri_hits = 0;
    for (u32 i = 0; i < RAY_REF; ++i) {
        const f64 o[3] = { s->ox[i], s->oy[i], s->oz[i] }, d[3] = { s->dx[i], s->dy[i], s->dz[i] };
        for (u32 b = 0; b < BOX_N; ++b) {
            const f64 lo[3] = { s->bminx[b], s->bminy[b], s->bminz[b] }, hi[3] = { s->bmaxx[b], s->bmaxy[b], s->bmaxz[b] };
            f64 t0 = 0.0, t1 = 1e30;
            for (u32 k = 0; k < 3; ++k) {
                const f64 inv = 1.0 / d[k];
                f64 a = (lo[k] - o[k]) * inv, bb = (hi[k] - o[k]) * inv;
                if (a > bb) { const f64 t = a; a = bb; bb = t; }
                t0 = a > t0 ? a : t0;
                t1 = bb < t1 ? bb : t1;
            }
            s->expect_box_hits += t0 <= t1;
        }
        for (u32 t = 0; t < TRI_N; ++t) {
            const f64 e1[3] = { s->e1[t][0], s->e1[t][1], s->e1[t][2] }, e2[3] = { s->e2[t][0], s->e2[t][1], s->e2[t][2] };
            const f64 pv[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
            const f64 det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (fabs(det) < 1e-12) continue;
            const f64 inv = 1.0 / det;
            const f64 tv[3] = { o[0] - s->v0[t][0], o[1] - s->v0[t][1], o[2] - s->v0[t][2] };
            const f64 u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (u < 0.0 || u > 1.0) continue;
            const f64 qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
            const f64 v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
            if (v < 0.0 || u + v > 1.0) continue;
            s->expect_tri_hits += (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv > 0.0;
        }
    }
}

// Slab test of rays [r0, r1) against every box; returns the hit count.
static u64 ray_box(const RayScene* s, u32 r0, u32 r1) {
    u64 hits = 0;
    for (u32 i = r0; i < r1; ++i) {
        const f32 ox = s->ox[i], oy = s->oy[i], oz = s->oz[i];
        const f32 ix = 1.0f / s->dx[i], iy = 1.0f / s->dy[i], iz = 1.0f / s->dz[i];
        u32 h = 0;
        for (u32 b = 0; b < BOX_N; ++b) {
            const f32 ax = (s->bminx[b] - ox) * ix, bx = (s->bmaxx[b] - ox) * ix;
            const f32 ay = (s->bminy[b] - oy) * iy, by = (s->bmaxy[b] - oy) * iy;
            const f32 az = (s->bminz[b] - oz) * iz, bz = (s->bmaxz[b] - oz) * iz;
            const f32 t0 = fmaxf(fmaxf(fminf(ax, bx), fminf(ay, by)), fmaxf(fminf(az, bz), 0.0f));
            const f32 t1 = fminf(fminf(fmaxf(ax, bx), fmaxf(ay, by)), fmaxf(az, bz));
            h += t0 <= t1;
        }
        hits += h;
    }
    return hits;
}

// Moller-Trumbore of rays [r0, r1) against every triangle (hits in front of the origin).
static u64 ray_tri(const RayScene* s, u32 r0, u32 r1) {
    u64 hits = 0;
    for (u32 i = r0; i < r1; ++i) {
        const f32 o[3] = { s->ox[i], s->oy[i], s->oz[i] }, d[3] = { s->dx[i], s->dy[i], s->dz[i] };
        u32 h = 0;
        for (u32 t = 0; t < TRI_N; ++t) {
            const f32* e1 = s->e1[t];
            const f32* e2 = s->e2[t];
            const f32 pv0 = d[1] * e2[2] - d[2] * e2[1], pv1 = d[2] * e2[0] - d[0] * e2[2], pv2 = d[0] * e2[1] - d[1] * e2[0];
            const f32 det = e1[0] * pv0 + e1[1] * pv1 + e1[2] * pv2;
            const f32 inv = 1.0f / det;
            const f32 tv0 = o[0] - s->v0[t][0], tv1 = o[1] - s->v0[t][1], tv2 = o[2] - s->v0[t][2];
            const f32 u = (tv0 * pv0 + tv1 * pv1 + tv2 * pv2) * inv;
            const f32 qv0 = tv1 * e1[2] - tv2 * e1[1], qv1 = tv2 * e1[0] - tv0 * e1[2], qv2 = tv0 * e1[1] - tv1 * e1[0];
            const f32 v = (d[0] * qv0 + d[1] * qv1 + d[2] * qv2) * inv;
            const f32 tt = (e2[0] * qv0 + e2[1] * qv1 + e2[2] * qv2) * inv;
            h += (fabsf(det) >= 1e-12f) & (u >= 0.0f) & (u <= 1.0f) & (v >= 0.0f) & (u + v <= 1.0f) & (tt > 0.0f);
        }
        hits += h;
    }
    return hits;
}

static RayScene* s_rays;

static f64 kern_raybox(void*, u32, u32) {
    do_not_optimize(ray_box(s_rays, 0, RAY_N));
    return (f64)RAY_N * BOX_N;
}

static f64 kern_raytri(void*, u32, u32) {
    do_not_optimize(ray_tri(s_rays, 0, RAY_N));
    return (f64)RAY_N * TRI_N;
}

// Float results may differ from the double reference on a handful of grazing hits.
static bool close_count(u64 got, u64 expect) {
    const u64 diff = got > expect ? got - expect : expect - got;
    return diff <= 2 + expect / 1000;
}

static bool ray_prepare(BenchResult* out) {
    if (!s_rays) {
        s_rays = (RayScene*)big_alloc(sizeof(RayScene));
        if (!s_rays) { res_fail(out, "out of memory"); return false; }
        ray_scene_init(s_rays);
    }
    return true;
}

void cpu_raybox(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!ray_prepare(out)) return;
    const u64 hits = ray_box(s_rays, 0, RAY_REF);
    if (s_rays->expect_box_hits == 0 || !close_count(hits, s_rays->expect_box_hits)) {
        res_fail(out, "ray-box hits %llu, reference %llu", hits, s_rays->expect_box_hits);
        return;
    }
    st_mt_test(kern_raybox, nullptr, "Mtests/s", 1e6, p, live, out, cancel);
}

void cpu_raytri(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!ray_prepare(out)) return;
    const u64 hits = ray_tri(s_rays, 0, RAY_REF);
    if (s_rays->expect_tri_hits == 0 || !close_count(hits, s_rays->expect_tri_hits)) {
        res_fail(out, "ray-triangle hits %llu, reference %llu", hits, s_rays->expect_tri_hits);
        return;
    }
    st_mt_test(kern_raytri, nullptr, "Mtests/s", 1e6, p, live, out, cancel);
}
