// CPU suite: test functions.
#include "cpu/cpu_internal.h"
#include "core/bench.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <windows.h>
#include <algorithm>
#include <immintrin.h>
#include <string.h>

// ---- suite ------------------------------------------------------------------------------

bool cpu_init(const BenchParams*, void** state, char*, u32) {
    topo_init();
    pool_init();
    *state = nullptr;
    return true;
}

void cpu_shutdown(void*) {}

// ---- integer / floating point ----------------------------------------------------------------

static constexpr u64 ITERS = 1u << 19;  // ~0.3-1.5 ms per call

static f64 kern_int_alu(void*, u32, u32) { return (f64)k_int_alu(ITERS); }
static f64 kern_int_mul(void*, u32, u32) { return (f64)k_int_mul(ITERS); }
static f64 kern_fma_ss(void*, u32, u32) { return (f64)k_fma_ss(ITERS); }
static f64 kern_fma_ps256(void*, u32, u32) { return (f64)k_fma_ps256(ITERS); }
static f64 kern_fma_pd256(void*, u32, u32) { return (f64)k_fma_pd256(ITERS); }
static f64 kern_fma_ps512(void*, u32, u32) { return (f64)k_fma_ps512(ITERS); }
static f64 kern_fma_pd512(void*, u32, u32) { return (f64)k_fma_pd512(ITERS); }

void cpu_int_alu(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    st_mt_test(kern_int_alu, nullptr, "Gops/s", 1e9, p, live, out, cancel);
}
void cpu_int_mul(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    st_mt_test(kern_int_mul, nullptr, "Gops/s", 1e9, p, live, out, cancel);
}
void cpu_fma_scalar(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    st_mt_test(kern_fma_ss, nullptr, "GFLOPS", 1e9, p, live, out, cancel);
}
void cpu_fma_avx2_f32(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.fma || !g_sys.cpu.avx2) { res_skip(out, "CPU lacks AVX2/FMA"); return; }
    st_mt_test(kern_fma_ps256, nullptr, "GFLOPS", 1e9, p, live, out, cancel);
}
void cpu_fma_avx2_f64(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.fma || !g_sys.cpu.avx2) { res_skip(out, "CPU lacks AVX2/FMA"); return; }
    st_mt_test(kern_fma_pd256, nullptr, "GFLOPS", 1e9, p, live, out, cancel);
}
void cpu_fma_avx512_f32(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.avx512f) { res_skip(out, "CPU lacks AVX-512F"); return; }
    st_mt_test(kern_fma_ps512, nullptr, "GFLOPS", 1e9, p, live, out, cancel);
}
void cpu_fma_avx512_f64(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.avx512f) { res_skip(out, "CPU lacks AVX-512F"); return; }
    st_mt_test(kern_fma_pd512, nullptr, "GFLOPS", 1e9, p, live, out, cancel);
}

// ---- SGEMM -------------------------------------------------------------------------------------

static f64 kern_gemm(void* ctx, u32 index, u32 count) { return gemm_run_part((GemmCtx*)ctx, index, count); }

void cpu_sgemm(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.fma || !g_sys.cpu.avx2) { res_skip(out, "CPU lacks AVX2/FMA"); return; }
    live_phase(live, "Allocating", "");
    GemmCtx* g = gemm_create(thread_count_for(p->thread_count));
    if (!g) { res_fail(out, "out of memory"); return; }
    st_mt_test(kern_gemm, g, "GFLOPS", 1e9, p, live, out, cancel);
    const f64 err = gemm_max_error(g);
    gemm_destroy(g);
    if (err > 1e-2) res_fail(out, "wrong result (max abs error %.3g)", err);
}

// ---- crypto ------------------------------------------------------------------------------------

static constexpr u64 CRYPTO_BYTES = 64 * KiB;   // per thread, L2 resident

struct CryptoCtx {
    u8* buf;            // per thread: src at index * 2 * CRYPTO_BYTES, dst right after
    AesKey key;
    bool vaes512;
};

static f64 kern_sha(void* ctx, u32 index, u32) {
    CryptoCtx* c = (CryptoCtx*)ctx;
    u32 st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    sha256_blocks(st, c->buf + (u64)index * 2 * CRYPTO_BYTES, CRYPTO_BYTES);
    do_not_optimize(st[0]);
    return (f64)CRYPTO_BYTES;
}

static f64 kern_aes(void* ctx, u32 index, u32) {
    CryptoCtx* c = (CryptoCtx*)ctx;
    u8* src = c->buf + (u64)index * 2 * CRYPTO_BYTES;
    if (c->vaes512) aes_ctr_vaes512(&c->key, index * 1000003ull, src, src + CRYPTO_BYTES, CRYPTO_BYTES);
    else aes_ctr_ni(&c->key, index * 1000003ull, src, src + CRYPTO_BYTES, CRYPTO_BYTES);
    clobber_memory();
    return (f64)CRYPTO_BYTES;
}

static bool crypto_ctx_init(CryptoCtx* c) {
    c->buf = (u8*)big_alloc((u64)MAX_LP * 2 * CRYPTO_BYTES);
    if (!c->buf) return false;
    for (u64 i = 0; i < (u64)MAX_LP * 2 * CRYPTO_BYTES; ++i) c->buf[i] = (u8)(i * 2654435761u >> 13);
    static const u8 key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    aes128_expand(key, &c->key);
    return true;
}

void cpu_sha256(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.sha) { res_skip(out, "CPU lacks SHA extensions"); return; }
    if (!sha256_selftest()) { res_fail(out, "SHA-256 known-answer test failed"); return; }
    CryptoCtx c = {};
    if (!crypto_ctx_init(&c)) { res_fail(out, "out of memory"); return; }
    st_mt_test(kern_sha, &c, "GB/s", 1e9, p, live, out, cancel);
    big_free(c.buf);
}

void cpu_aes(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.aes) { res_skip(out, "CPU lacks AES-NI"); return; }
    CryptoCtx c = {};
    c.vaes512 = g_sys.cpu.vaes && g_sys.cpu.avx512f;
    if (!aes_selftest(c.vaes512)) { res_fail(out, "AES known-answer test failed"); return; }
    if (!crypto_ctx_init(&c)) { res_fail(out, "out of memory"); return; }
    st_mt_test(kern_aes, &c, "GB/s", 1e9, p, live, out, cancel);
    g_metrics[out->first + 0].name = c.vaes512 ? "Single thread (VAES-512)" : "Single thread (AES-NI)";
    g_metrics[out->first + 1].name = c.vaes512 ? "Multi-thread (VAES-512)" : "Multi-thread (AES-NI)";
    big_free(c.buf);
}

// ---- sort --------------------------------------------------------------------------------------

static constexpr u32 SORT_N = 16u << 20;

struct SortJob {
    u32* src;       // random input
    u32* a;         // working buffers
    u32* b;
    u32 bounds[MAX_LP + 1];
    u32 chunks;
};

static void sort_chunk(u32 index, u32, void* user) {
    SortJob* j = (SortJob*)user;
    std::sort(j->a + j->bounds[index], j->a + j->bounds[index + 1]);
}

// Merge chunk pairs (2i, 2i+1) from a into b.
static void merge_pair(u32 index, u32, void* user) {
    SortJob* j = (SortJob*)user;
    const u32 c0 = 2 * index, lo = j->bounds[c0];
    if (c0 + 1 >= j->chunks) {      // odd chunk out: copy through
        memcpy(j->b + lo, j->a + lo, (u64)(j->bounds[c0 + 1] - lo) * sizeof(u32));
        return;
    }
    const u32 mid = j->bounds[c0 + 1], hi = j->bounds[c0 + 2];
    std::merge(j->a + lo, j->a + mid, j->a + mid, j->a + hi, j->b + lo);
}

// Parallel merge sort of j->a (result ends up in j->a).
static void parallel_sort(SortJob* j, u32 threads) {
    j->chunks = threads;
    for (u32 i = 0; i <= threads; ++i) j->bounds[i] = (u32)((u64)SORT_N * i / threads);
    pool_run(threads, sort_chunk, j);
    while (j->chunks > 1) {
        const u32 pairs = (j->chunks + 1) / 2;
        pool_run(pairs, merge_pair, j);
        u32 nb[MAX_LP + 1];
        for (u32 i = 0; i < pairs; ++i) nb[i] = j->bounds[2 * i];
        nb[pairs] = SORT_N;
        memcpy(j->bounds, nb, sizeof(u32) * (pairs + 1));
        j->chunks = pairs;
        u32* t = j->a; j->a = j->b; j->b = t;
    }
}

void cpu_sort(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Allocating", "");
    SortJob j = {};
    u32* mem = (u32*)big_alloc((u64)SORT_N * 3 * sizeof(u32));
    if (!mem) { res_fail(out, "out of memory"); return; }
    j.src = mem;
    u64 x = 0x853C49E6748FEA9Bull;
    for (u32 i = 0; i < SORT_N; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; j.src[i] = (u32)(x >> 16); }

    const f64 seconds = p->duration_ms / 1000.0;
    const u32 mt = thread_count_for(p->thread_count);
    f64 rates[2] = {};
    bool sorted_ok = true;
    for (u32 phase = 0; phase < 2 && !cancel->load(); ++phase) {
        const u32 threads = phase == 0 ? 1 : mt;
        live_phase(live, phase == 0 ? "Single thread" : "Multi-thread", "Melem/s");
        f64 total_time = 0.0;
        u32 reps = 0;
        // Buffers are pre-faulted, so no warm-up rep; output is verified after the first rep (untimed).
        for (u32 rep = 0; (total_time < seconds || reps == 0) && !cancel->load(); ++rep) {
            j.a = mem + SORT_N;
            j.b = mem + 2 * (u64)SORT_N;
            memcpy(j.a, j.src, (u64)SORT_N * sizeof(u32));
            const f64 t0 = timer_now();
            if (threads == 1) std::sort(j.a, j.a + SORT_N);
            else parallel_sort(&j, threads);
            const f64 dt = timer_now() - t0;
            if (rep == 0) sorted_ok &= std::is_sorted(j.a, j.a + SORT_N);
            total_time += dt;
            ++reps;
            live_sample(live, SORT_N / dt / 1e6);
            const f64 frac = total_time / seconds;
            live_progress(live, 0.5f * phase + 0.5f * (f32)(frac < 1.0 ? frac : 1.0));
        }
        rates[phase] = reps ? SORT_N * (f64)reps / total_time / 1e6 : 0.0;
    }
    big_free(mem);
    res_begin(out, RESULT_VALUES);
    res_add(out, "Single thread", "Melem/s", rates[0]);
    res_add(out, "Multi-thread", "Melem/s", rates[1]);
    res_add(out, "MT / ST", "x", rates[0] > 0.0 ? rates[1] / rates[0] : 0.0);
    if (!sorted_ok) res_fail(out, "output not sorted");
}

// ---- prime sieve ---------------------------------------------------------------------------------
// Segmented, odd-only Eratosthenes up to 1e9. Index i represents the odd number 2i+1.

static constexpr u64 SIEVE_N = 1000000000ull;
static constexpr u64 SIEVE_PRIMES = 50847534ull;     // pi(1e9)
static constexpr u32 SIEVE_SEG = 32 * 1024;          // bytes per segment (L1D)

struct SieveJob {
    u32* primes;        // odd base primes <= sqrt(N)
    u32 prime_count;
    u8* seg;            // per-thread segment buffers
    u32* next;          // per-thread next-multiple index per prime
    std::atomic<u64> count;
};

static void sieve_range(u32 index, u32 count, void* user) {
    SieveJob* j = (SieveJob*)user;
    const u64 total = SIEVE_N / 2;
    // Segment-aligned split of [0, total).
    const u64 segs = (total + SIEVE_SEG - 1) / SIEVE_SEG;
    const u64 i0 = segs * index / count * SIEVE_SEG;
    u64 i1 = segs * (index + 1) / count * SIEVE_SEG;
    if (i1 > total) i1 = total;
    u8* seg = j->seg + (u64)index * SIEVE_SEG;
    u32* next = j->next + (u64)index * j->prime_count;

    for (u32 k = 0; k < j->prime_count; ++k) {
        const u64 pr = j->primes[k];
        u64 m = (pr * pr - 1) / 2;
        if (m < i0) m += (i0 - m + pr - 1) / pr * pr;
        next[k] = (u32)m;
    }
    u64 found = 0;
    for (u64 lo = i0; lo < i1; lo += SIEVE_SEG) {
        const u64 hi = lo + SIEVE_SEG < i1 ? lo + SIEVE_SEG : i1;
        const u32 len = (u32)(hi - lo);
        memset(seg, 0, len);
        if (lo == 0) seg[0] = 1;    // 1 is not prime
        for (u32 k = 0; k < j->prime_count; ++k) {
            const u32 pr = j->primes[k];
            u64 m = next[k];
            for (; m < hi; m += pr) seg[m - lo] = 1;
            next[k] = (u32)m;
        }
        u32 zeros = 0;
        for (u32 i = 0; i < len; ++i) zeros += seg[i] == 0;
        found += zeros;
    }
    j->count.fetch_add(found, std::memory_order_relaxed);
}

void cpu_sieve(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    // Base primes up to sqrt(1e9) = 31623 with a tiny plain sieve.
    static u8 small[31624];
    static u32 primes[4000];
    memset(small, 0, sizeof(small));
    u32 np = 0;
    for (u32 i = 3; i < sizeof(small); i += 2) {
        if (small[i]) continue;
        primes[np++] = i;
        for (u32 m = i * i; m < sizeof(small); m += 2 * i) small[m] = 1;
    }

    SieveJob j = {};
    j.primes = primes;
    j.prime_count = np;
    j.seg = (u8*)big_alloc((u64)MAX_LP * SIEVE_SEG);
    j.next = (u32*)big_alloc((u64)MAX_LP * np * sizeof(u32));
    if (!j.seg || !j.next) { big_free(j.seg); big_free(j.next); res_fail(out, "out of memory"); return; }

    const f64 seconds = p->duration_ms / 1000.0;
    const u32 mt = thread_count_for(p->thread_count);
    f64 rates[2] = {};
    bool count_ok = true;
    for (u32 phase = 0; phase < 2 && !cancel->load(); ++phase) {
        const u32 threads = phase == 0 ? 1 : mt;
        live_phase(live, phase == 0 ? "Single thread" : "Multi-thread", "Mnum/s");
        f64 total_time = 0.0;
        u32 reps = 0;
        while ((total_time < seconds || reps == 0) && !cancel->load()) {
            j.count.store(0);
            const f64 t0 = timer_now();
            pool_run(threads, sieve_range, &j);
            const f64 dt = timer_now() - t0;
            count_ok &= j.count.load() + 1 == SIEVE_PRIMES;    // +1 for the prime 2
            total_time += dt;
            ++reps;
            live_sample(live, SIEVE_N / dt / 1e6);
            const f64 frac = total_time / seconds;
            live_progress(live, 0.5f * phase + 0.5f * (f32)(frac < 1.0 ? frac : 1.0));
        }
        rates[phase] = SIEVE_N * (f64)reps / total_time / 1e6;
    }
    big_free(j.seg);
    big_free(j.next);
    res_begin(out, RESULT_VALUES);
    res_add(out, "Single thread", "Mnum/s", rates[0]);
    res_add(out, "Multi-thread", "Mnum/s", rates[1]);
    res_add(out, "MT / ST", "x", rates[0] > 0.0 ? rates[1] / rates[0] : 0.0);
    if (!count_ok) res_fail(out, "wrong prime count");
}

// ---- core-to-core latency -------------------------------------------------------------------------
// Two threads pinned to LPs a and b bounce a counter on one cache line. One-way latency = round trip / 2.

struct alignas(64) PingLine { std::atomic<u32> v; char pad[60]; };

struct C2CJob {
    u32 lp[2];
    u32 warmup, iters;
    PingLine* line;
    f64 seconds;
};

static void c2c_task(u32 index, u32, void* user) {
    C2CJob* j = (C2CJob*)user;
    pin_current_thread(j->lp[index]);
    std::atomic<u32>& v = j->line->v;
    const u32 total = j->warmup + j->iters;
    if (index == 0) {
        f64 t0 = 0.0;
        for (u32 i = 0; i < total; ++i) {
            if (i == j->warmup) t0 = timer_now();
            v.store(2 * i + 1, std::memory_order_release);
            while (v.load(std::memory_order_acquire) != 2 * i + 2) _mm_pause();
        }
        j->seconds = timer_now() - t0;
    } else {
        for (u32 i = 0; i < total; ++i) {
            while (v.load(std::memory_order_acquire) != 2 * i + 1) _mm_pause();
            v.store(2 * i + 2, std::memory_order_release);
        }
    }
}

void cpu_c2c(void*, const BenchParams*, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u32 n = g_topo.lp_count;
    if (n < 2) { res_skip(out, "single logical processor"); return; }
    live_phase(live, "Ping-pong between logical processors", "ns");
    static PingLine line;
    static f64 lat[MAX_LP][MAX_LP];
    memset(lat, 0, sizeof(lat));
    const u32 pairs = n * (n - 1) / 2;
    u32 done = 0;
    for (u32 a = 0; a < n && !cancel->load(); ++a) {
        for (u32 b = a + 1; b < n && !cancel->load(); ++b) {
            C2CJob j = { { a, b }, 2000, 20000, &line, 0.0 };
            line.v.store(0);
            pool_run(2, c2c_task, &j);
            lat[a][b] = lat[b][a] = j.seconds / (2.0 * j.iters) * 1e9;
            live_sample(live, lat[a][b]);
            live_progress(live, (f32)++done / pairs);
        }
    }
    res_begin(out, RESULT_VALUES);
    for (u32 r = 0; r < n; ++r)
        for (u32 c = 0; c < n; ++c) res_add(out, "One-way latency", "ns", lat[r][c]);
    res_matrix(out, (u16)n, (u16)n);
}

// ---- thread scaling --------------------------------------------------------------------------------

void cpu_scaling(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.fma || !g_sys.cpu.avx2) { res_skip(out, "CPU lacks AVX2/FMA"); return; }
    const u32 max_t = thread_count_for(p->thread_count);
    u32 counts[16], n = 0;
    for (u32 t = 1; t < max_t; t *= 2) counts[n++] = t;
    counts[n++] = max_t;

    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Threads", "threads");
    live_phase(live, "AVX2 FP32 FMA at increasing thread counts", "GFLOPS");
    const f64 seconds = p->duration_ms / 1000.0 / 2.0;
    for (u32 i = 0; i < n && !cancel->load(); ++i) {
        const f64 rate = run_throughput(kern_fma_ps256, nullptr, counts[i], seconds, 1e9, live, cancel,
                                        (f32)i / n, (f32)(i + 1) / n);
        res_add(out, "AVX2 FP32 FMA", "GFLOPS", rate, (f64)counts[i]);
    }
}
