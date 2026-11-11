// Single-precision GEMM, Goto-style blocking with a 6x16 AVX2 FMA micro-kernel.
// Threads split C by 6-row strips; each thread packs its own A and B blocks.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/threads.h"
#include <immintrin.h>
#include <math.h>
#include <string.h>

constexpr u32 MR = 6, NR = 16;
constexpr u32 KC = 240, NC = 480, MC = 120;     // Ap = 115 KB (L2), Bp = 460 KB, B sliver 15 KB (L1)
static_assert(GEMM_N % MR == 0 && GEMM_N % NR == 0 && GEMM_N % KC == 0 && GEMM_N % NC == 0, "GEMM blocking");

struct GemmCtx {
    f32* a;
    f32* b;
    f32* c;
    f32* pack_a[MAX_LP];
    f32* pack_b[MAX_LP];
    u32 threads;
};

GemmCtx* gemm_create(u32 max_threads) {
    static GemmCtx g;
    memset(&g, 0, sizeof(g));
    const u64 mat = (u64)GEMM_N * GEMM_N * sizeof(f32);
    g.a = (f32*)big_alloc(mat);
    g.b = (f32*)big_alloc(mat);
    g.c = (f32*)big_alloc(mat);
    g.threads = max_threads;
    bool ok = g.a && g.b && g.c;
    for (u32 t = 0; t < max_threads && ok; ++t) {
        g.pack_a[t] = (f32*)big_alloc((u64)MC * KC * sizeof(f32));
        g.pack_b[t] = (f32*)big_alloc((u64)KC * NC * sizeof(f32));
        ok = g.pack_a[t] && g.pack_b[t];
    }
    if (!ok) { gemm_destroy(&g); return nullptr; }

    u64 x = 0x243F6A8885A308D3ull;
    for (u64 i = 0; i < (u64)GEMM_N * GEMM_N; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        g.a[i] = (f32)((x >> 40) & 0xFFFF) / 32768.0f - 1.0f;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        g.b[i] = (f32)((x >> 40) & 0xFFFF) / 32768.0f - 1.0f;
    }
    return &g;
}

void gemm_destroy(GemmCtx* g) {
    if (!g) return;
    big_free(g->a); big_free(g->b); big_free(g->c);
    for (u32 t = 0; t < MAX_LP; ++t) { big_free(g->pack_a[t]); big_free(g->pack_b[t]); }
    memset(g, 0, sizeof(*g));
}

// C[6x16] += Ap[kc x 6] * Bp[kc x 16]
static void micro_6x16(u32 kc, const f32* a, const f32* b, f32* c, u32 ldc) {
    __m256 c00 = _mm256_loadu_ps(c + 0 * ldc), c01 = _mm256_loadu_ps(c + 0 * ldc + 8);
    __m256 c10 = _mm256_loadu_ps(c + 1 * ldc), c11 = _mm256_loadu_ps(c + 1 * ldc + 8);
    __m256 c20 = _mm256_loadu_ps(c + 2 * ldc), c21 = _mm256_loadu_ps(c + 2 * ldc + 8);
    __m256 c30 = _mm256_loadu_ps(c + 3 * ldc), c31 = _mm256_loadu_ps(c + 3 * ldc + 8);
    __m256 c40 = _mm256_loadu_ps(c + 4 * ldc), c41 = _mm256_loadu_ps(c + 4 * ldc + 8);
    __m256 c50 = _mm256_loadu_ps(c + 5 * ldc), c51 = _mm256_loadu_ps(c + 5 * ldc + 8);
    for (u32 k = 0; k < kc; ++k, a += MR, b += NR) {
        const __m256 b0 = _mm256_load_ps(b), b1 = _mm256_load_ps(b + 8);
        __m256 av;
        av = _mm256_broadcast_ss(a + 0); c00 = _mm256_fmadd_ps(av, b0, c00); c01 = _mm256_fmadd_ps(av, b1, c01);
        av = _mm256_broadcast_ss(a + 1); c10 = _mm256_fmadd_ps(av, b0, c10); c11 = _mm256_fmadd_ps(av, b1, c11);
        av = _mm256_broadcast_ss(a + 2); c20 = _mm256_fmadd_ps(av, b0, c20); c21 = _mm256_fmadd_ps(av, b1, c21);
        av = _mm256_broadcast_ss(a + 3); c30 = _mm256_fmadd_ps(av, b0, c30); c31 = _mm256_fmadd_ps(av, b1, c31);
        av = _mm256_broadcast_ss(a + 4); c40 = _mm256_fmadd_ps(av, b0, c40); c41 = _mm256_fmadd_ps(av, b1, c41);
        av = _mm256_broadcast_ss(a + 5); c50 = _mm256_fmadd_ps(av, b0, c50); c51 = _mm256_fmadd_ps(av, b1, c51);
    }
    _mm256_storeu_ps(c + 0 * ldc, c00); _mm256_storeu_ps(c + 0 * ldc + 8, c01);
    _mm256_storeu_ps(c + 1 * ldc, c10); _mm256_storeu_ps(c + 1 * ldc + 8, c11);
    _mm256_storeu_ps(c + 2 * ldc, c20); _mm256_storeu_ps(c + 2 * ldc + 8, c21);
    _mm256_storeu_ps(c + 3 * ldc, c30); _mm256_storeu_ps(c + 3 * ldc + 8, c31);
    _mm256_storeu_ps(c + 4 * ldc, c40); _mm256_storeu_ps(c + 4 * ldc + 8, c41);
    _mm256_storeu_ps(c + 5 * ldc, c50); _mm256_storeu_ps(c + 5 * ldc + 8, c51);
}

f64 gemm_run_part(GemmCtx* g, u32 index, u32 count) {
    const u32 N = GEMM_N, strips = N / MR;
    const u32 m0 = MR * (strips * index / count), m1 = MR * (strips * (index + 1) / count);
    if (m0 == m1) return 0.0;
    f32* ap = g->pack_a[index];
    f32* bp = g->pack_b[index];
    memset(g->c + (u64)m0 * N, 0, (u64)(m1 - m0) * N * sizeof(f32));

    for (u32 jc = 0; jc < N; jc += NC) {
        for (u32 pc = 0; pc < N; pc += KC) {
            // Pack B[pc:pc+KC, jc:jc+NC] as NR-wide column panels, k-major.
            for (u32 jr = 0; jr < NC; jr += NR)
                for (u32 k = 0; k < KC; ++k)
                    memcpy(bp + jr * KC + k * NR, g->b + (u64)(pc + k) * N + jc + jr, NR * sizeof(f32));
            for (u32 ic = m0; ic < m1; ic += MC) {
                const u32 mc = (m1 - ic) < MC ? (m1 - ic) : MC;
                // Pack A[ic:ic+mc, pc:pc+KC] as MR-row strips, k-major.
                for (u32 ir = 0; ir < mc; ir += MR)
                    for (u32 k = 0; k < KC; ++k)
                        for (u32 r = 0; r < MR; ++r)
                            ap[ir * KC + k * MR + r] = g->a[(u64)(ic + ir + r) * N + pc + k];
                for (u32 jr = 0; jr < NC; jr += NR)
                    for (u32 ir = 0; ir < mc; ir += MR)
                        micro_6x16(KC, ap + ir * KC, bp + jr * KC, g->c + (u64)(ic + ir) * N + jc + jr, N);
            }
        }
    }
    return 2.0 * (m1 - m0) * (f64)N * N;
}

f64 gemm_max_error(const GemmCtx* g) {
    const u32 N = GEMM_N;
    f64 max_err = 0.0;
    u64 x = 0x9E3779B97F4A7C15ull;
    for (u32 s = 0; s < 64; ++s) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        const u32 i = (u32)(x % N), j = (u32)((x >> 32) % N);
        f64 ref = 0.0;
        for (u32 k = 0; k < N; ++k) ref += (f64)g->a[(u64)i * N + k] * g->b[(u64)k * N + j];
        const f64 err = fabs(ref - g->c[(u64)i * N + j]);
        if (err > max_err) max_err = err;
    }
    return max_err;
}
