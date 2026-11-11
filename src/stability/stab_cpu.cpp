// CPU stability tests: self-checking kernels on every logical processor (STAB_CPU), one core at a time
// (STAB_CORES), and cache-sized pattern tests (STAB_CACHE).
//
// Known answers: each thread owns a small block of random data (in L1 with its mirror copy) and runs a kernel
// over it many times; every run's result is compared with a reference computed once at the start by a
// different instruction path (scalar, unoptimized code: `optnone`). FMA and multiply / add are correctly rounded
// in every encoding, so the scalar reference must match the SIMD result bit for bit. The integer kernel's
// reference is the same arithmetic in unoptimized scalar code. The large-data kernel streams a shared buffer
// from RAM with per-slice references.
#include "stability/stab_internal.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/timer.h"
#include <windows.h>
#include <immintrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#pragma clang fp contract(off)

constexpr u32 SMALL_F = 2048;           // floats per small block (8 KiB, + 8 KiB mirror)
constexpr u32 SMALL_I = 1024;           // u64 per integer block (8 KiB, + mirror)
constexpr u32 ROUNDS = 64;              // passes over the block per kernel call
constexpr u32 SLICE_F = 1u << 20;       // large kernel: floats per slice (4 MiB)
constexpr u64 LARGE_MAX = 256 * MiB;    // shared streaming buffer, at most
constexpr f64 MIX_SECONDS = 4.0;        // mixed mode: time per kernel

static u32 round_offset(u32 r, u32 n) { return ((r * 37u + 11u) % (n / 16)) * 16; }   // 64-byte aligned rotation

// ---- kernels (hot) -----------------------------------------------------------------------------------------
// out[k * W + l] = lane l of accumulator k. Step = K accumulators x W lanes, K = 8.

static void k_sse(const f32* d, f32* out) {
    __m128 acc[8];
    for (u32 k = 0; k < 8; ++k) acc[k] = _mm_setzero_ps();
    for (u32 r = 0; r < ROUNDS; ++r) {
        const f32* b = d + round_offset(r, SMALL_F);
        for (u32 i = 0; i < SMALL_F; i += 32)
            for (u32 k = 0; k < 8; ++k)
                acc[k] = _mm_add_ps(acc[k], _mm_mul_ps(_mm_load_ps(d + i + 4 * k), _mm_loadu_ps(b + i + 4 * k)));
    }
    for (u32 k = 0; k < 8; ++k) _mm_storeu_ps(out + 4 * k, acc[k]);
}

static void k_avx2(const f32* d, f32* out) {
    __m256 acc[8];
    for (u32 k = 0; k < 8; ++k) acc[k] = _mm256_setzero_ps();
    for (u32 r = 0; r < ROUNDS; ++r) {
        const f32* b = d + round_offset(r, SMALL_F);
        for (u32 i = 0; i < SMALL_F; i += 64)
            for (u32 k = 0; k < 8; ++k)
                acc[k] = _mm256_fmadd_ps(_mm256_load_ps(d + i + 8 * k), _mm256_loadu_ps(b + i + 8 * k), acc[k]);
    }
    for (u32 k = 0; k < 8; ++k) _mm256_storeu_ps(out + 8 * k, acc[k]);
}

__attribute__((target("avx512f"))) static void k_avx512(const f32* d, f32* out) {
    __m512 acc[8];
    for (u32 k = 0; k < 8; ++k) acc[k] = _mm512_setzero_ps();
    for (u32 r = 0; r < ROUNDS; ++r) {
        const f32* b = d + round_offset(r, SMALL_F);
        for (u32 i = 0; i < SMALL_F; i += 128)
            for (u32 k = 0; k < 8; ++k)
                acc[k] = _mm512_fmadd_ps(_mm512_load_ps(d + i + 16 * k), _mm512_loadu_ps(b + i + 16 * k), acc[k]);
    }
    for (u32 k = 0; k < 8; ++k) _mm512_storeu_ps(out + 16 * k, acc[k]);
}

static u64 rotl(u64 x, u32 s) { return (x << s) | (x >> (64 - s)); }

static void k_int(const u64* d, u64* out) {
    u64 acc[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (u32 r = 0; r < ROUNDS; ++r) {
        const u64* b = d + round_offset(r, SMALL_I);
#pragma clang loop vectorize(disable) interleave(disable)
        for (u32 i = 0; i < SMALL_I; i += 8)
            for (u32 k = 0; k < 8; ++k) acc[k] = rotl(acc[k] ^ d[i + k], 23) * 0x9E3779B97F4A7C15ull + b[i + k];
    }
    for (u32 k = 0; k < 8; ++k) out[k] = acc[k];
}

// Large data: one slice streamed from RAM, 8 AVX2 accumulators against constant multipliers.
static void k_large(const f32* s, f32* out) {
    __m256 acc[8];
    __m256 c[8];
    for (u32 k = 0; k < 8; ++k) { acc[k] = _mm256_setzero_ps(); c[k] = _mm256_set1_ps(0.75f + k * 0.03125f); }
    for (u32 i = 0; i < SLICE_F; i += 64)
        for (u32 k = 0; k < 8; ++k) acc[k] = _mm256_fmadd_ps(_mm256_load_ps(s + i + 8 * k), c[k], acc[k]);
    for (u32 k = 0; k < 8; ++k) _mm256_storeu_ps(out + 8 * k, acc[k]);
}

// ---- references (scalar, unoptimized, computed once) ----------------------------------------------------

__attribute__((noinline, optnone)) static void ref_float(const f32* d, u32 step, bool fused, f32* out) {
    for (u32 j = 0; j < step; ++j) out[j] = 0.0f;
    for (u32 r = 0; r < ROUNDS; ++r) {
        const f32* b = d + round_offset(r, SMALL_F);
        for (u32 i = 0; i < SMALL_F; i += step)
            for (u32 j = 0; j < step; ++j) {
                const __m128 a = _mm_set_ss(d[i + j]), m = _mm_set_ss(b[i + j]), c = _mm_set_ss(out[j]);
                out[j] = _mm_cvtss_f32(fused ? _mm_fmadd_ss(a, m, c) : _mm_add_ss(c, _mm_mul_ss(a, m)));
            }
    }
}

__attribute__((noinline, optnone)) static void ref_int(const u64* d, u64* out) {
    u64 acc[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    for (u32 r = 0; r < ROUNDS; ++r) {
        const u64* b = d + round_offset(r, SMALL_I);
        for (u32 i = 0; i < SMALL_I; i += 8)
            for (u32 k = 0; k < 8; ++k) acc[k] = rotl(acc[k] ^ d[i + k], 23) * 0x9E3779B97F4A7C15ull + b[i + k];
    }
    for (u32 k = 0; k < 8; ++k) out[k] = acc[k];
}

__attribute__((noinline, optnone)) static void ref_large(const f32* s, f32* out) {
    for (u32 j = 0; j < 64; ++j) out[j] = 0.0f;
    for (u32 i = 0; i < SLICE_F; i += 64)
        for (u32 j = 0; j < 64; ++j) {
            const __m128 c = _mm_set_ss(0.75f + (j / 8) * 0.03125f);
            out[j] = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(s[i + j]), c, _mm_set_ss(out[j])));
        }
}

static f32 rand_float(u64 seed) {      // [0.5, 1): no denormals, no overflow in the sums
    const u32 bits = 0x3F000000u | (u32)(stab_mix(seed) & 0x7FFFFFu);
    f32 f;
    memcpy(&f, &bits, 4);
    return f;
}

// ---- shared worker ---------------------------------------------------------------------------------------

struct CpuShared {
    StabTest test;
    f64 deadline, start;
    const u32* lps;
    CpuKernel kernel;
    bool avx512;
    f32* large;                 // shared streaming buffer (CK_LARGE / mixed), null if unavailable
    u32 slices;
    f32* large_ref;             // 64 per slice
    std::atomic<bool> injected;
};

struct alignas(64) ThreadData {
    f32 f[SMALL_F * 2];         // data + mirror (round offsets read past the end)
    u64 q[SMALL_I * 2];
    f32 ref_sse[32], ref_avx2[64], ref_avx512[128];
    u64 ref_q[8];
    f32 out[128];
    u64 out_q[8];
};

static const char* const KERNEL_PHASE[CK_COUNT] = { "Mixed", "Integer", "SSE multiply-add", "AVX2 FMA",
                                                    "AVX-512 FMA", "Large data (RAM)" };

static void init_thread_data(ThreadData* t, u64 seed) {
    for (u32 i = 0; i < SMALL_F; ++i) t->f[i] = t->f[i + SMALL_F] = rand_float(seed * 0x10000 + i);
    for (u32 i = 0; i < SMALL_I; ++i) t->q[i] = t->q[i + SMALL_I] = stab_mix(seed * 0x20000 + i);
    ref_float(t->f, 32, false, t->ref_sse);
    ref_float(t->f, 64, true, t->ref_avx2);
    ref_float(t->f, 128, true, t->ref_avx512);
    ref_int(t->q, t->ref_q);
}

// Kernel to use now (mixed: by elapsed time, the same on every thread).
static CpuKernel pick_kernel(const CpuShared* s) {
    if (s->kernel != CK_MIXED) return s->kernel;
    CpuKernel list[5];
    u32 n = 0;
    list[n++] = CK_INT;
    list[n++] = CK_SSE;
    list[n++] = CK_AVX2;
    if (s->avx512) list[n++] = CK_AVX512;
    if (s->large) list[n++] = CK_LARGE;
    return list[(u32)((timer_now() - s->start) / MIX_SECONDS) % n];
}

static void compare_f32(StabTest test, i32 lp, const char* what, const f32* got, const f32* want, u32 n) {
    for (u32 j = 0; j < n; ++j) {
        u32 a, b;
        memcpy(&a, &got[j], 4);
        memcpy(&b, &want[j], 4);
        if (a != b) stab_error(test, lp, -1, j, what, b, a);
    }
}

// Runs kernels until `until` (or the test's deadline), checking every call. Returns operations verified.
static f64 cpu_work(CpuShared* s, ThreadData* t, u32 index, f64 until) {
    const i32 lp = (i32)s->lps[index];
    const f64 small_ops = (f64)ROUNDS * SMALL_F;    // multiply-adds (float lanes) or mixing steps x 8 per call
    f64 ops = 0.0;
    u32 calls = 0;
    while (!stab_done(s->deadline) && timer_now() < until) {
        const CpuKernel k = pick_kernel(s);
        if (index == 0 && s->test != STAB_CORES) stab_phase(s->test, KERNEL_PHASE[k]);     // cores: shows the core
        // Dev self-test: corrupt one result bit once, after a second of load.
        const bool inject = g_stab.cfg.inject && index == 0 && timer_now() - s->start > 1.0 && !s->injected.exchange(true);
        for (u32 rep = 0; rep < 16 && !g_stab.stop.load(std::memory_order_relaxed); ++rep, ++calls) {
            switch (k) {
                case CK_INT:
                    k_int(t->q, t->out_q);
                    if (inject && rep == 0) t->out_q[3] ^= 1ull << 41;
                    for (u32 j = 0; j < 8; ++j)
                        if (t->out_q[j] != t->ref_q[j]) stab_error(s->test, lp, -1, j, "integer", t->ref_q[j], t->out_q[j]);
                    ops += (f64)ROUNDS * SMALL_I;
                    break;
                case CK_SSE:
                    k_sse(t->f, t->out);
                    if (inject && rep == 0) ((u32*)t->out)[5] ^= 1u << 9;
                    compare_f32(s->test, lp, "SSE multiply-add", t->out, t->ref_sse, 32);
                    ops += small_ops;
                    break;
                case CK_AVX512:
                    if (s->avx512) {
                        k_avx512(t->f, t->out);
                        if (inject && rep == 0) ((u32*)t->out)[100] ^= 1u << 2;
                        compare_f32(s->test, lp, "AVX-512 FMA", t->out, t->ref_avx512, 128);
                        ops += small_ops;
                        break;
                    }
                    [[fallthrough]];
                case CK_AVX2:
                    k_avx2(t->f, t->out);
                    if (inject && rep == 0) ((u32*)t->out)[17] ^= 1u << 22;
                    compare_f32(s->test, lp, "AVX2 FMA", t->out, t->ref_avx2, 64);
                    ops += small_ops;
                    break;
                case CK_LARGE: {
                    if (!s->large) break;     // not picked without the buffer
                    const u32 slice = (index * 7 + calls * 13) % s->slices;
                    k_large(s->large + (u64)slice * SLICE_F, t->out);
                    if (inject && rep == 0) ((u32*)t->out)[0] ^= 1u << 30;
                    compare_f32(s->test, lp, "Large data FMA", t->out, s->large_ref + slice * 64, 64);
                    ops += SLICE_F;
                    rep = 16;       // ~1 ms per slice: check the clock after each
                    break;
                }
                default: break;
            }
        }
    }
    return ops;
}

static void large_ref_worker(u32 index, void* user) {
    CpuShared* s = (CpuShared*)user;
    const u32 n = g_topo.lp_count;
    for (u32 slice = index; slice < s->slices; slice += n) {
        f32* p = s->large + (u64)slice * SLICE_F;
        for (u32 i = 0; i < SLICE_F; ++i) p[i] = rand_float(0xABCD0000000ull + (u64)slice * SLICE_F + i);
        ref_large(p, s->large_ref + slice * 64);
    }
}

// Shared setup for STAB_CPU / STAB_CORES. The large buffer is skipped when memory is short.
static bool cpu_setup(CpuShared* s, StabTest test, f64 deadline, const u32* lps) {
    s->test = test;
    s->deadline = deadline;
    s->start = timer_now();
    s->lps = lps;
    s->kernel = g_stab.cfg.cpu_kernel;
    s->avx512 = g_sys.cpu.avx512f;
    s->large = nullptr;
    s->injected.store(false);
    if (s->kernel == CK_AVX512 && !s->avx512) {
        stab_fail_setup(test, "this CPU has no AVX-512");
        return false;
    }
    if (s->kernel == CK_MIXED || s->kernel == CK_LARGE) {
        MEMORYSTATUSEX ms = { sizeof(ms) };
        GlobalMemoryStatusEx(&ms);
        u64 bytes = LARGE_MAX;
        while (bytes > 16 * MiB && bytes > ms.ullAvailPhys / 8) bytes /= 2;
        s->slices = (u32)(bytes / (SLICE_F * 4));
        s->large = (f32*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        s->large_ref = (f32*)VirtualAlloc(nullptr, (u64)s->slices * 64 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s->large || !s->large_ref) {
            if (s->large) VirtualFree(s->large, 0, MEM_RELEASE);
            if (s->large_ref) VirtualFree(s->large_ref, 0, MEM_RELEASE);
            s->large = nullptr;
            if (s->kernel == CK_LARGE) { stab_fail_setup(test, "out of memory for the large-data buffer"); return false; }
        } else {
            stab_phase(test, "Preparing data");
            u32 all[STAB_MAX_LP];
            for (u32 i = 0; i < g_topo.lp_count && i < STAB_MAX_LP; ++i) all[i] = g_topo.order[i];
            stab_run_threads(g_topo.lp_count, all, large_ref_worker, s);
            g_stab.tests[test].size_bytes = bytes;
        }
    }
    return true;
}

static void cpu_teardown(CpuShared* s) {
    if (s->large) VirtualFree(s->large, 0, MEM_RELEASE);
    if (s->large_ref) VirtualFree(s->large_ref, 0, MEM_RELEASE);
    s->large = nullptr;
}

static ThreadData* alloc_thread_data(u32 lp) {
    ThreadData* t = (ThreadData*)VirtualAlloc(nullptr, sizeof(ThreadData), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (t) init_thread_data(t, 0x5EED0000ull + lp);
    return t;
}

static void cpu_worker(u32 index, void* user) {
    CpuShared* s = (CpuShared*)user;
    ThreadData* t = alloc_thread_data(s->lps[index]);
    if (!t) return;
    const f64 ops = cpu_work(s, t, index, INFINITY);
    stab_work_mt(s->test, ops / 1e9);
    VirtualFree(t, 0, MEM_RELEASE);
}

void stab_cpu_run(f64 deadline) {
    CpuShared s;
    u32 lps[STAB_MAX_LP];
    const u32 n = g_topo.lp_count < STAB_MAX_LP ? g_topo.lp_count : STAB_MAX_LP;
    for (u32 i = 0; i < n; ++i) lps[i] = g_topo.order[i];
    if (!cpu_setup(&s, STAB_CPU, deadline, lps)) return;
    stab_run_threads(n, lps, cpu_worker, &s);
    cpu_teardown(&s);
}

// ---- per-core cycling ------------------------------------------------------------------------------------

struct CoresArg { CpuShared* s; f64 core_seconds; };

static void cores_worker(u32, void* user) {
    CoresArg* a = (CoresArg*)user;
    CpuShared* s = a->s;
    // One thread moves from core to core (re-pinning itself), so its data stays the same object.
    u32 first_lp[STAB_MAX_LP];
    u32 cores = 0;
    for (u32 c = 0; c < g_topo.core_count && cores < STAB_MAX_LP; ++c)
        for (u32 lp = 0; lp < g_topo.lp_count; ++lp)
            if (g_topo.core[lp] == c) { first_lp[cores++] = lp; break; }
    ThreadData* t = alloc_thread_data(0);
    if (!t || !cores) return;
    u32 lp_slot[1];
    s->lps = lp_slot;
    f64 ops = 0.0;
    for (u32 i = 0; !stab_done(s->deadline); ++i) {
        const u32 lp = first_lp[i % cores];
        lp_slot[0] = lp;
        static char core_names[STAB_MAX_LP][24];     // phase strings must outlive the run
        snprintf(core_names[lp], sizeof(core_names[lp]), "Core %u (LP %u)", g_topo.core[lp], lp);
        stab_phase(STAB_CORES, core_names[lp]);
        pin_current_thread(lp);
        Sleep(0);       // let the scheduler move us before timing starts
        const f64 t0 = timer_now();
        ops += cpu_work(s, t, 0, t0 + a->core_seconds);
        StabTestState* ts = &g_stab.tests[STAB_CORES];
        ts->lp_seconds[lp].store(ts->lp_seconds[lp].load() + (f32)(timer_now() - t0), std::memory_order_relaxed);
        stab_work_mt(STAB_CORES, ops / 1e9);
        ops = 0.0;
    }
    VirtualFree(t, 0, MEM_RELEASE);
}

void stab_cores_run(f64 deadline) {
    CpuShared s;
    u32 lp0 = g_topo.order[0];
    if (!cpu_setup(&s, STAB_CORES, deadline, &lp0)) return;
    CoresArg a = { &s, g_stab.cfg.core_seconds ? (f64)g_stab.cfg.core_seconds : 60.0 };
    stab_run_threads(1, &lp0, cores_worker, &a);
    cpu_teardown(&s);
}

// ---- cache test -----------------------------------------------------------------------------------------
// Per thread: a buffer sized to a share of L1, L2 or L3, filled from an LCG stream, then checked + inverted and
// checked again. Levels rotate every few seconds (all threads together).

struct CacheShared {
    f64 deadline, start;
    const u32* lps;
    u64 level_bytes[3];
    std::atomic<bool> injected;
};

static const char* const CACHE_PHASE[3] = { "L1", "L2", "L3" };

static void cache_worker(u32 index, void* user) {
    CacheShared* s = (CacheShared*)user;
    const i32 lp = (i32)s->lps[index];
    const u64 max_bytes = s->level_bytes[2] > s->level_bytes[1] ? s->level_bytes[2] : s->level_bytes[1];
    u64* buf = (u64*)VirtualAlloc(nullptr, max_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    u64 seed = stab_mix(0xCAC4E000ull + lp);
    f64 bytes = 0.0;
    while (!stab_done(s->deadline)) {
        const u32 level = (u32)((timer_now() - s->start) / MIX_SECONDS) % 3;
        if (index == 0) stab_phase(STAB_CACHE, CACHE_PHASE[level]);
        const u64 n = s->level_bytes[level] / 8;
        for (u32 rep = 0; rep < 8; ++rep) {
            seed = stab_mix(seed);
            u64 x = seed;
            for (u64 i = 0; i < n; ++i) { buf[i] = x; x = x * 6364136223846793005ull + 1442695040888963407ull; }
            if (g_stab.cfg.inject && index == 0 && timer_now() - s->start > 1.0 && !s->injected.exchange(true))
                buf[n / 2] ^= 1ull << 13;
            x = seed;
            for (u64 i = 0; i < n; ++i) {     // check, write the complement
                const u64 v = buf[i];
                if (v != x) stab_error(STAB_CACHE, lp, -1, (u64)(uintptr_t)&buf[i], CACHE_PHASE[level], x, v);
                buf[i] = ~x;
                x = x * 6364136223846793005ull + 1442695040888963407ull;
            }
            x = seed;
            for (u64 i = 0; i < n; ++i) {
                const u64 v = buf[i];
                if (v != ~x) stab_error(STAB_CACHE, lp, -1, (u64)(uintptr_t)&buf[i], CACHE_PHASE[level], ~x, v);
                x = x * 6364136223846793005ull + 1442695040888963407ull;
            }
            bytes += 2.0 * n * 8;
        }
    }
    stab_work_mt(STAB_CACHE, bytes / 1e9);
    VirtualFree(buf, 0, MEM_RELEASE);
}

void stab_cache_run(f64 deadline) {
    CacheShared s;
    s.deadline = deadline;
    s.start = timer_now();
    s.injected.store(false);
    const CpuInfo* c = &g_sys.cpu;
    const u32 lps = g_topo.lp_count < STAB_MAX_LP ? g_topo.lp_count : STAB_MAX_LP;
    const u32 per_core = g_topo.core_count ? (lps + g_topo.core_count - 1) / g_topo.core_count : 1;   // SMT threads share L1 / L2
    // Leave a quarter of each level for the stack, the code and the other thread's working data.
    s.level_bytes[0] = ((c->l1d_kib ? c->l1d_kib : 32) * KiB * 3 / 4 / per_core) & ~63ull;
    s.level_bytes[1] = ((c->l2_kib ? c->l2_kib : 512) * KiB * 3 / 4 / per_core) & ~63ull;
    s.level_bytes[2] = ((c->l3_kib ? c->l3_kib : 8192) * KiB * 3 / 4 / lps) & ~63ull;
    u32 order[STAB_MAX_LP];
    for (u32 i = 0; i < lps; ++i) order[i] = g_topo.order[i];
    s.lps = order;
    g_stab.tests[STAB_CACHE].size_bytes = s.level_bytes[0] + s.level_bytes[1] + s.level_bytes[2];
    stab_run_threads(lps, order, cache_worker, &s);
}
