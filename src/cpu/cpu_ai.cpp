// "AI inference" throughput: small int8 (u8 x i8 -> i32) and BF16 (-> FP32) matrix multiplies with the
// dot-product instructions inference engines use: AVX-512 VNNI / AVX-VNNI / AVX2 (vpmaddubsw) for int8,
// AVX-512 BF16 for BF16 (runtime-gated). Inputs are small exact values, so results must match a scalar
// reference bit for bit.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/throughput.h"
#include <immintrin.h>
#include <string.h>

// C[M][N] = A[M][K] * B[K][N]. B is packed in K-groups: int8 [K/4][N][4], BF16 [K/2][N][2], so one vector
// load gives 4 (int8) or 2 (BF16) consecutive k for a run of columns. Micro-tile: 6 rows x 2 vectors.
static constexpr u32 AM = 192, AN = 256, AK = 256;
static constexpr f64 AI_OPS = 2.0 * AM * AN * AK;     // multiply + add per MAC

enum AiPath { I8_AVX512_VNNI, I8_AVX_VNNI, I8_AVX2, BF16_AVX512 };

struct AiCtx {
    u8* a;              // int8: u8 [AM][AK]; bf16: u16 [AM][AK]
    u8* b;              // packed B
    u8* c;              // per thread: [AM][AN] i32 / f32
    u8* ref;            // reference C
    AiPath path;
};

static u8* c_of(AiCtx* x, u32 index) { return x->c + (u64)index * AM * AN * 4; }
static inline u32 a_word(const u8* row, u32 byte) { u32 v; memcpy(&v, row + byte, 4); return v; }

// ---- int8

__attribute__((target("avx512f,avx512vnni"))) static void gemm_i8_vnni512(const u8* a, const u8* b, i32* c) {
    for (u32 i0 = 0; i0 < AM; i0 += 6) {
        for (u32 n0 = 0; n0 < AN; n0 += 32) {
            __m512i acc[6][2];
            for (u32 r = 0; r < 6; ++r) acc[r][0] = acc[r][1] = _mm512_setzero_si512();
            for (u32 kk = 0; kk < AK / 4; ++kk) {
                const u8* bp = b + ((u64)kk * AN + n0) * 4;
                const __m512i b0 = _mm512_loadu_si512(bp), b1 = _mm512_loadu_si512(bp + 64);
                for (u32 r = 0; r < 6; ++r) {
                    const __m512i av = _mm512_set1_epi32((int)a_word(a + (u64)(i0 + r) * AK, 4 * kk));
                    acc[r][0] = _mm512_dpbusd_epi32(acc[r][0], av, b0);
                    acc[r][1] = _mm512_dpbusd_epi32(acc[r][1], av, b1);
                }
            }
            for (u32 r = 0; r < 6; ++r) {
                _mm512_storeu_si512(c + (u64)(i0 + r) * AN + n0, acc[r][0]);
                _mm512_storeu_si512(c + (u64)(i0 + r) * AN + n0 + 16, acc[r][1]);
            }
        }
    }
}

__attribute__((target("avxvnni"))) static void gemm_i8_avxvnni(const u8* a, const u8* b, i32* c) {
    for (u32 i0 = 0; i0 < AM; i0 += 6) {
        for (u32 n0 = 0; n0 < AN; n0 += 16) {
            __m256i acc[6][2];
            for (u32 r = 0; r < 6; ++r) acc[r][0] = acc[r][1] = _mm256_setzero_si256();
            for (u32 kk = 0; kk < AK / 4; ++kk) {
                const u8* bp = b + ((u64)kk * AN + n0) * 4;
                const __m256i b0 = _mm256_loadu_si256((const __m256i*)bp), b1 = _mm256_loadu_si256((const __m256i*)(bp + 32));
                for (u32 r = 0; r < 6; ++r) {
                    const __m256i av = _mm256_set1_epi32((int)a_word(a + (u64)(i0 + r) * AK, 4 * kk));
                    acc[r][0] = _mm256_dpbusd_avx_epi32(acc[r][0], av, b0);
                    acc[r][1] = _mm256_dpbusd_avx_epi32(acc[r][1], av, b1);
                }
            }
            for (u32 r = 0; r < 6; ++r) {
                _mm256_storeu_si256((__m256i*)(c + (u64)(i0 + r) * AN + n0), acc[r][0]);
                _mm256_storeu_si256((__m256i*)(c + (u64)(i0 + r) * AN + n0 + 8), acc[r][1]);
            }
        }
    }
}

// AVX2: vpmaddubsw (u8 x i8 pairs -> s16, saturating; our values never saturate) + vpmaddwd by 1.
TARGET_AVX2 static void gemm_i8_avx2(const u8* a, const u8* b, i32* c) {
    const __m256i ones = _mm256_set1_epi16(1);
    for (u32 i0 = 0; i0 < AM; i0 += 6) {
        for (u32 n0 = 0; n0 < AN; n0 += 16) {
            __m256i acc[6][2];
            for (u32 r = 0; r < 6; ++r) acc[r][0] = acc[r][1] = _mm256_setzero_si256();
            for (u32 kk = 0; kk < AK / 4; ++kk) {
                const u8* bp = b + ((u64)kk * AN + n0) * 4;
                const __m256i b0 = _mm256_loadu_si256((const __m256i*)bp), b1 = _mm256_loadu_si256((const __m256i*)(bp + 32));
                for (u32 r = 0; r < 6; ++r) {
                    const __m256i av = _mm256_set1_epi32((int)a_word(a + (u64)(i0 + r) * AK, 4 * kk));
                    acc[r][0] = _mm256_add_epi32(acc[r][0], _mm256_madd_epi16(_mm256_maddubs_epi16(av, b0), ones));
                    acc[r][1] = _mm256_add_epi32(acc[r][1], _mm256_madd_epi16(_mm256_maddubs_epi16(av, b1), ones));
                }
            }
            for (u32 r = 0; r < 6; ++r) {
                _mm256_storeu_si256((__m256i*)(c + (u64)(i0 + r) * AN + n0), acc[r][0]);
                _mm256_storeu_si256((__m256i*)(c + (u64)(i0 + r) * AN + n0 + 8), acc[r][1]);
            }
        }
    }
}

// ---- BF16

__attribute__((target("avx512f,avx512bf16"))) static void gemm_bf16_avx512(const u8* a, const u8* b, f32* c) {
    for (u32 i0 = 0; i0 < AM; i0 += 6) {
        for (u32 n0 = 0; n0 < AN; n0 += 32) {
            __m512 acc[6][2];
            for (u32 r = 0; r < 6; ++r) acc[r][0] = acc[r][1] = _mm512_setzero_ps();
            for (u32 kk = 0; kk < AK / 2; ++kk) {
                const u8* bp = b + ((u64)kk * AN + n0) * 4;      // 2 bf16 per column
                const __m512bh b0 = (__m512bh)_mm512_loadu_si512(bp), b1 = (__m512bh)_mm512_loadu_si512(bp + 64);
                for (u32 r = 0; r < 6; ++r) {
                    const __m512bh av = (__m512bh)_mm512_set1_epi32((int)a_word(a + (u64)(i0 + r) * AK * 2, 4 * kk));
                    acc[r][0] = _mm512_dpbf16_ps(acc[r][0], av, b0);
                    acc[r][1] = _mm512_dpbf16_ps(acc[r][1], av, b1);
                }
            }
            for (u32 r = 0; r < 6; ++r) {
                _mm512_storeu_ps(c + (u64)(i0 + r) * AN + n0, acc[r][0]);
                _mm512_storeu_ps(c + (u64)(i0 + r) * AN + n0 + 16, acc[r][1]);
            }
        }
    }
}

static void ai_run(AiCtx* x, u8* c) {
    switch (x->path) {
        case I8_AVX512_VNNI: gemm_i8_vnni512(x->a, x->b, (i32*)c); break;
        case I8_AVX_VNNI: gemm_i8_avxvnni(x->a, x->b, (i32*)c); break;
        case I8_AVX2: gemm_i8_avx2(x->a, x->b, (i32*)c); break;
        case BF16_AVX512: gemm_bf16_avx512(x->a, x->b, (f32*)c); break;
    }
}

static f64 kern_ai(void* ctx, u32 index, u32) {
    AiCtx* x = (AiCtx*)ctx;
    ai_run(x, c_of(x, index));
    return AI_OPS;
}

// ---- setup + reference

static u64 ai_rng(u64* s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static u16 to_bf16(f32 v) { u32 u; memcpy(&u, &v, 4); return (u16)(u >> 16); }     // exact for our values

static bool ai_setup(AiCtx* x, AiPath path, u32 threads) {
    const bool bf = path == BF16_AVX512;
    x->path = path;
    x->a = (u8*)big_alloc((u64)AM * AK * 2);
    x->b = (u8*)big_alloc((u64)AK * AN * 2);
    x->c = (u8*)big_alloc((u64)threads * AM * AN * 4);
    x->ref = (u8*)big_alloc((u64)AM * AN * 4);
    if (!x->a || !x->b || !x->c || !x->ref) return false;
    static i8 bi[AK][AN];
    static f32 af[AM][AK], bfv[AK][AN];
    u64 s = 0xA1B2C3D4E5F60718ull;
    // int8: A in [0, 127], B in [-64, 63] (vpmaddubsw pair sums stay below 32767).
    // BF16: multiples of 1/8 in [-2, 2]: products and 256-term sums are exact in FP32.
    for (u32 i = 0; i < AM; ++i)
        for (u32 k = 0; k < AK; ++k) {
            const u64 r = ai_rng(&s);
            if (bf) { af[i][k] = (f32)((i32)(r % 33) - 16) / 8.0f; ((u16*)x->a)[(u64)i * AK + k] = to_bf16(af[i][k]); }
            else x->a[(u64)i * AK + k] = (u8)(r % 128);
        }
    for (u32 k = 0; k < AK; ++k)
        for (u32 n = 0; n < AN; ++n) {
            const u64 r = ai_rng(&s);
            if (bf) {
                bfv[k][n] = (f32)((i32)(r % 33) - 16) / 8.0f;
                ((u16*)x->b)[((u64)(k / 2) * AN + n) * 2 + (k & 1)] = to_bf16(bfv[k][n]);
            } else {
                bi[k][n] = (i8)((i32)(r % 128) - 64);
                x->b[((u64)(k / 4) * AN + n) * 4 + (k & 3)] = (u8)bi[k][n];
            }
        }
    for (u32 i = 0; i < AM; ++i)
        for (u32 n = 0; n < AN; ++n) {
            if (bf) {
                f32 sum = 0.0f;
                for (u32 k = 0; k < AK; ++k) sum += af[i][k] * bfv[k][n];
                ((f32*)x->ref)[(u64)i * AN + n] = sum;
            } else {
                i32 sum = 0;
                for (u32 k = 0; k < AK; ++k) sum += (i32)x->a[(u64)i * AK + k] * bi[k][n];
                ((i32*)x->ref)[(u64)i * AN + n] = sum;
            }
        }
    return true;
}

static void ai_free(AiCtx* x) {
    big_free(x->a);
    big_free(x->b);
    big_free(x->c);
    big_free(x->ref);
}

static void ai_test(AiPath path, const char* unit, const BenchParams* p, BenchLive* live, BenchResult* out,
                    const CancelFlag* cancel) {
    AiCtx x = {};
    const u32 mt = thread_count_for(p->thread_count);
    if (!ai_setup(&x, path, mt)) { ai_free(&x); res_fail(out, "out of memory"); return; }
    ai_run(&x, c_of(&x, 0));
    const bool ok = memcmp(c_of(&x, 0), x.ref, (u64)AM * AN * 4) == 0;
    if (ok) st_mt_test(kern_ai, &x, unit, 1e9, p, live, out, cancel);
    const bool still_ok = ok && memcmp(c_of(&x, 0), x.ref, (u64)AM * AN * 4) == 0;
    ai_free(&x);
    if (!still_ok) { res_fail(out, "result differs from the scalar reference"); return; }
    static const char* const ST[] = { "Single thread (AVX-512 VNNI)", "Single thread (AVX-VNNI)", "Single thread (AVX2)",
                                      "Single thread (AVX-512 BF16)" };
    static const char* const MT[] = { "Multi-thread (AVX-512 VNNI)", "Multi-thread (AVX-VNNI)", "Multi-thread (AVX2)",
                                      "Multi-thread (AVX-512 BF16)" };
    g_metrics[out->first + 0].name = ST[path];
    g_metrics[out->first + 1].name = MT[path];
}

void cpu_ai_int8(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.avx2) { res_skip(out, "CPU lacks AVX2"); return; }
    const AiPath path = g_sys.cpu.avx512vnni ? I8_AVX512_VNNI : g_cpu_ext.avx_vnni ? I8_AVX_VNNI : I8_AVX2;
    ai_test(path, "GOPS", p, live, out, cancel);
}

void cpu_ai_bf16(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_cpu_ext.avx512_bf16) { res_skip(out, "CPU lacks AVX-512 BF16"); return; }
    ai_test(BF16_AVX512, "GFLOPS", p, live, out, cancel);
}
