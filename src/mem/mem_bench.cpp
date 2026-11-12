// Memory suite: DRAM bandwidth (read/write/copy/triad), load latency curve, cache bandwidth sweeps.
#include "mem/mem_internal.h"
#include "mem/mem_kernels.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Never take more than a quarter of the RAM that is free when the suite starts.
static constexpr u64 ARENA_TARGET = 1536 * MiB;
static constexpr u64 ARENA_MIN = 256 * MiB;

static MemState s_mem;

// Keep the arena resident while the suite runs: under memory pressure from other processes Windows trims the
// working set and random-access latency turns into soft page faults (a 1 GiB chase read 1.4 us instead of
// ~120 ns with ~8 GB free and many busy processes). A soft minimum (no hard limit) needs no special rights.
static SIZE_T s_ws_min, s_ws_max;
static DWORD s_ws_flags;
static bool s_ws_raised;

static void ws_raise(u64 bytes) {
    if (!GetProcessWorkingSetSizeEx(GetCurrentProcess(), &s_ws_min, &s_ws_max, &s_ws_flags)) return;
    const SIZE_T want = s_ws_min + (SIZE_T)bytes + 64 * MiB;   // + pointer-chase permutations
    const SIZE_T max = s_ws_max > want + 64 * MiB ? s_ws_max : want + 64 * MiB;
    s_ws_raised = SetProcessWorkingSetSizeEx(GetCurrentProcess(), want, max, s_ws_flags) != 0;
}

static void ws_restore() {
    if (s_ws_raised) SetProcessWorkingSetSizeEx(GetCurrentProcess(), s_ws_min, s_ws_max, s_ws_flags);
    s_ws_raised = false;
}

bool mem_init(const BenchParams*, void** state, char* error, u32 error_size) {
    topo_init();
    pool_init();
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    u64 bytes = ms.ullAvailPhys / 4;
    if (bytes > ARENA_TARGET) bytes = ARENA_TARGET;
    bytes = bytes / (256 * MiB) * (256 * MiB);
    if (bytes < ARENA_MIN) {
        snprintf(error, error_size, "not enough free RAM (%llu MiB available)", ms.ullAvailPhys / MiB);
        return false;
    }
    ws_raise(bytes);
    s_mem.arena = (u8*)big_alloc(bytes);
    if (!s_mem.arena) {
        ws_restore();
        snprintf(error, error_size, "failed to allocate %llu MiB", bytes / MiB);
        return false;
    }
    s_mem.bytes = bytes;
    *state = &s_mem;
    return true;
}

void mem_shutdown(void* state) {
    MemState* m = (MemState*)state;
    ws_restore();
    big_free(m->arena);
    m->arena = nullptr;
    m->bytes = 0;
}

// ---- bandwidth ------------------------------------------------------------------------------

enum BwOp : u8 { BW_READ, BW_WRITE, BW_WRITE_NT, BW_COPY, BW_COPY_NT, BW_TRIAD };

struct BwCtx {
    u8* a;
    u8* b;
    u8* c;
    u64 bytes;      // per array
    BwOp op;
};

// Per-thread slice of [0, bytes), 4 KiB aligned.
static void slice(u64 bytes, u32 index, u32 count, u64* off, u64* len) {
    const u64 pages = bytes / 4096;
    *off = pages * index / count * 4096;
    *len = pages * (index + 1) / count * 4096 - *off;
}

static f64 kern_bw(void* ctx, u32 index, u32 count) {
    const BwCtx* c = (const BwCtx*)ctx;
    u64 off, len;
    slice(c->bytes, index, count, &off, &len);
    switch (c->op) {
        case BW_READ:     k_mem_read(c->a + off, len); return (f64)len;
        case BW_WRITE:    k_mem_write(c->a + off, len); return (f64)len;
        case BW_WRITE_NT: k_mem_write_nt(c->a + off, len); return (f64)len;
        case BW_COPY:     k_mem_copy(c->a + off, c->b + off, len); return 2.0 * len;
        case BW_COPY_NT:  k_mem_copy_nt(c->a + off, c->b + off, len); return 2.0 * len;
        case BW_TRIAD:    k_mem_triad((f32*)(c->a + off), (const f32*)(c->b + off), (const f32*)(c->c + off), 3.0f, len);
                          return 3.0 * len;
    }
    return 0.0;
}

struct BwPhase { const char* name; BwOp op; bool mt; };

static void run_bw_phases(MemState* m, const BwPhase* ph, u32 n, const BenchParams* p, BenchLive* live,
                          BenchResult* out, const CancelFlag* cancel) {
    const f64 seconds = p->duration_ms / 1000.0;
    const u32 mt = thread_count_for(p->thread_count);
    const u64 total = m->bytes < GiB ? m->bytes : GiB;     // working set >> L3 (16 MiB)
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < n && !cancel->load(); ++i) {
        BwCtx c = {};
        c.op = ph[i].op;
        if (c.op == BW_COPY || c.op == BW_COPY_NT) {
            c.bytes = total / 2;
            c.a = m->arena;
            c.b = m->arena + c.bytes;
        } else if (c.op == BW_TRIAD) {
            c.bytes = m->bytes / 3 / 4096 * 4096;
            c.a = m->arena;
            c.b = m->arena + c.bytes;
            c.c = m->arena + 2 * c.bytes;
        } else {
            c.bytes = total;
            c.a = m->arena;
        }
        live_phase(live, ph[i].name, "GB/s");
        const f64 rate = run_throughput(kern_bw, &c, ph[i].mt ? mt : 1, seconds, 1e9, live, cancel,
                                        (f32)i / n, (f32)(i + 1) / n);
        res_add(out, ph[i].name, "GB/s", rate);
    }
}

static f64 dram_peak_gbs() {
    f64 peak = 0.0;
    for (u32 i = 0; i < g_sys.ram.module_count; ++i) {
        const RamModule* r = &g_sys.ram.modules[i];
        const u32 mts = r->configured_mts ? r->configured_mts : r->speed_mts;
        peak += mts * 8.0 / 1000.0;     // 64-bit module: 8 bytes per transfer
    }
    return peak;
}

void mem_read(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const BwPhase ph[] = { { "Single thread", BW_READ, false }, { "Multi-thread", BW_READ, true } };
    run_bw_phases((MemState*)state, ph, ARRAY_COUNT(ph), p, live, out, cancel);
    const f64 peak = dram_peak_gbs();
    if (peak > 0.0 && out->count == 2) {
        res_add(out, "Theoretical DRAM peak", "GB/s", peak);
        res_add(out, "Multi-thread / peak", "%", g_metrics[out->first + 1].value / peak * 100.0);
    }
}

void mem_write(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const BwPhase ph[] = {
        { "Single thread", BW_WRITE, false },
        { "Multi-thread", BW_WRITE, true },
        { "Single thread, non-temporal", BW_WRITE_NT, false },
        { "Multi-thread, non-temporal", BW_WRITE_NT, true },
    };
    run_bw_phases((MemState*)state, ph, ARRAY_COUNT(ph), p, live, out, cancel);
}

void mem_copy(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const BwPhase ph[] = {
        { "Single thread", BW_COPY, false },
        { "Multi-thread", BW_COPY, true },
        { "Single thread, non-temporal", BW_COPY_NT, false },
        { "Multi-thread, non-temporal", BW_COPY_NT, true },
    };
    run_bw_phases((MemState*)state, ph, ARRAY_COUNT(ph), p, live, out, cancel);
}

void mem_triad(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const BwPhase ph[] = { { "Single thread", BW_TRIAD, false }, { "Multi-thread", BW_TRIAD, true } };
    run_bw_phases((MemState*)state, ph, ARRAY_COUNT(ph), p, live, out, cancel);
}

// ---- latency ----------------------------------------------------------------------------------
// Random single-cycle pointer chase (Sattolo), one node per 64-byte line, on the best core.

static constexpr u32 LAT_MAX_POINTS = 24;

struct LatJob {
    u8* buf;
    u64 buf_bytes;
    u32* perm;
    f64 seconds_per_point;
    BenchLive* live;
    const CancelFlag* cancel;
    u32 count;
    u64 sizes[LAT_MAX_POINTS];
    f64 ns[LAT_MAX_POINTS];
};

f64 mem_point_seconds(const BenchParams* p, f64 div) {
    const f64 s = p->duration_ms / 1000.0 / div;
    return s < 0.25 ? 0.25 : s;
}

void chase_build(u8* buf, u64 bytes, u32* perm, u64* rng) {
    const u64 lines = bytes / 64;
    u64 r = *rng;
    for (u64 i = 0; i < lines; ++i) perm[i] = (u32)i;
    for (u64 i = lines - 1; i > 0; --i) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        const u64 k = r % i;
        const u32 t = perm[i]; perm[i] = perm[k]; perm[k] = t;
    }
    for (u64 i = 0; i < lines; ++i) *(u8**)(buf + i * 64) = buf + (u64)perm[i] * 64;
    *rng = r;
}

f64 chase_ns(u8* buf, u64 bytes, f64 seconds, const CancelFlag* cancel) {
    const u64 lines = bytes / 64;
    void* p = buf;
    const u64 chunk = 1u << 14;
    p = k_mem_chase(p, (lines < (1u << 20) ? lines : (1u << 20)) / 16 * 16 + 16);   // warm-up
    u64 steps = 0;
    const f64 t0 = timer_now();
    f64 now = t0;
    while (now - t0 < seconds && !cancel->load(std::memory_order_relaxed)) {
        p = k_mem_chase(p, chunk);
        steps += chunk;
        now = timer_now();
    }
    do_not_optimize(p);
    return steps ? (now - t0) / steps * 1e9 : 0.0;
}

static void latency_task(u32, u32, void* user) {
    LatJob* j = (LatJob*)user;
    u64 rng = 0x2545F4914F6CDD1Dull;
    for (u32 s = 0; s < j->count && !j->cancel->load(); ++s) {
        chase_build(j->buf, j->sizes[s], j->perm, &rng);
        j->ns[s] = chase_ns(j->buf, j->sizes[s], j->seconds_per_point, j->cancel);
        live_sample(j->live, j->ns[s]);
        live_progress(j->live, (f32)(s + 1) / j->count);
    }
}

void mem_latency(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    MemState* m = (MemState*)state;
    static LatJob j;
    memset(&j, 0, sizeof(j));
    j.live = live;
    j.cancel = cancel;
    j.seconds_per_point = mem_point_seconds(p, 8.0);

    // Prefer large pages (TLB misses otherwise add to DRAM latency); fall back to the arena.
    const u64 max_bytes = m->bytes < GiB ? m->bytes : GiB;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    u8* large = ms.ullAvailPhys / 4 >= max_bytes ? (u8*)big_alloc_large(max_bytes) : nullptr;
    j.buf = large ? large : m->arena;
    j.buf_bytes = max_bytes;
    j.perm = (u32*)big_alloc(max_bytes / 64 * sizeof(u32));
    if (!j.perm) { big_free(large); res_fail(out, "out of memory"); return; }

    for (u64 sz = 4 * KiB; sz <= max_bytes && j.count < LAT_MAX_POINTS; sz *= 2) j.sizes[j.count++] = sz;
    live_phase(live, large ? "Pointer chase (2 MiB pages)" : "Pointer chase (4 KiB pages)", "ns");
    pool_run(1, latency_task, &j);      // pool worker 0 = best core

    big_free(j.perm);
    big_free(large);
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Working set", "bytes");
    const char* name = large ? "Latency (2 MiB pages)" : "Latency (4 KiB pages)";
    for (u32 s = 0; s < j.count && j.ns[s] > 0.0; ++s) res_add(out, name, "ns", j.ns[s], (f64)j.sizes[s]);
}

// ---- cache bandwidth sweep -------------------------------------------------------------------------
// Read bandwidth vs working-set size. MT: every thread reads its own private region of that size.

struct SweepCtx {
    u8* base;
    u64 size;       // per-thread working set
    u64 stride;     // distance between per-thread regions
};

static f64 kern_sweep(void* ctx, u32 index, u32) {
    const SweepCtx* c = (const SweepCtx*)ctx;
    const u8* p = c->base + (u64)index * c->stride;
    const u64 reps = c->size >= 4 * MiB ? 1 : 4 * MiB / c->size;
    for (u64 r = 0; r < reps; ++r) k_mem_read(p, c->size);
    return (f64)(reps * c->size);
}

static void run_sweep(MemState* m, bool multi, const BenchParams* p, BenchLive* live, BenchResult* out,
                      const CancelFlag* cancel) {
    const u32 threads = multi ? thread_count_for(p->thread_count) : 1;
    const u64 cap = m->bytes / threads;
    u64 sizes[20];
    u32 n = 0;
    for (u64 sz = 16 * KiB; sz <= 256 * MiB && sz <= cap && n < 20; sz *= 2) sizes[n++] = sz;

    const f64 seconds = mem_point_seconds(p, 8.0);
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, multi ? "Working set per thread" : "Working set", "bytes");
    live_phase(live, multi ? "Read sweep, all threads" : "Read sweep, one thread", "GB/s");
    for (u32 i = 0; i < n && !cancel->load(); ++i) {
        SweepCtx c = { m->arena, sizes[i], (sizes[i] + 4095) / 4096 * 4096 };
        const f64 rate = run_throughput(kern_sweep, &c, threads, seconds, 1e9, live, cancel, (f32)i / n, (f32)(i + 1) / n);
        res_add(out, "Read", "GB/s", rate, (f64)sizes[i]);
    }
}

void mem_sweep_st(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_sweep((MemState*)state, false, p, live, out, cancel);
}

void mem_sweep_mt(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_sweep((MemState*)state, true, p, live, out, cancel);
}
