// Memory under load (Intel MLC style): loaded latency curve, read / write mix ratios, bandwidth vs thread
// count, and 4 KiB vs 2 MiB pages.
#include "mem/mem_internal.h"
#include "mem/mem_kernels.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/timer.h"
#include <windows.h>
#include <string.h>

// ---- loaded latency -----------------------------------------------------------------------------------
// Worker 0 (best core) chases pointers through a 256 MiB random cycle while one thread on every other physical
// core reads its own DRAM region with a delay loop after each 256 bytes. Fewer delay cycles = more load. SMT
// siblings stay idle so the latency core shares nothing but the memory system with the load.

static const u32 LOAD_DELAYS[] = { 100000, 20000, 10000, 5000, 2500, 1200, 600, 300, 150, 75, 0 };

struct alignas(64) LoadSlot {
    f64 bytes, t0, t1;
};

struct LoadJob {
    u8* chase;
    u64 chase_bytes;
    u8* gen;
    u64 gen_bytes;          // per generator thread
    u64 delay;
    f64 seconds;
    const CancelFlag* cancel;
    std::atomic<bool> measure, stop;
    f64 ns;
    LoadSlot slots[MAX_LP];
};

static void load_task(u32 index, u32, void* user) {
    LoadJob* j = (LoadJob*)user;
    if (index == 0) {
        Sleep(30);      // let the generators reach steady state
        j->measure.store(true);
        j->ns = chase_ns(j->chase, j->chase_bytes, j->seconds, j->cancel);
        j->stop.store(true);
        return;
    }
    LoadSlot* s = &j->slots[index];
    u8* base = j->gen + (u64)(index - 1) * j->gen_bytes;
    // Chunk per call: ~1 MiB without delay, 256 B at the largest delay, so stopping stays quick.
    u64 chunk = 256 * (4096 / (1 + j->delay / 16));
    if (chunk < 256) chunk = 256;
    u64 off = 0;
    bool measuring = false;
    f64 bytes = 0.0;
    while (!j->stop.load(std::memory_order_relaxed)) {
        if (off + chunk > j->gen_bytes) off = 0;
        k_mem_read_delay(base + off, chunk, j->delay);
        off += chunk;
        if (measuring) bytes += (f64)chunk;
        else if (j->measure.load(std::memory_order_relaxed)) { measuring = true; s->t0 = timer_now(); }
    }
    s->t1 = timer_now();
    s->bytes = measuring ? bytes : 0.0;
}

void mem_lat_loaded(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    MemState* m = (MemState*)state;
    u32 threads = thread_count_for(p->thread_count);
    if (threads > g_topo.core_count) threads = g_topo.core_count;    // one per physical core
    if (threads < 2) { res_skip(out, "needs at least 2 cores"); return; }
    const u32 gens = threads - 1;
    static LoadJob j;
    memset(&j, 0, sizeof(j));
    j.chase = m->arena;
    j.chase_bytes = m->bytes / 4 < 256 * MiB ? m->bytes / 4 : 256 * MiB;
    j.gen = m->arena + j.chase_bytes;
    j.gen_bytes = (m->bytes - j.chase_bytes) / gens / 4096 * 4096;
    if (j.gen_bytes > 256 * MiB) j.gen_bytes = 256 * MiB;
    j.seconds = mem_point_seconds(p, 8.0);
    j.cancel = cancel;
    u32* perm = (u32*)big_alloc(j.chase_bytes / 64 * sizeof(u32));
    if (!perm) { res_fail(out, "out of memory"); return; }
    u64 rng = 0x2545F4914F6CDD1Dull;
    chase_build(j.chase, j.chase_bytes, perm, &rng);
    big_free(perm);

    const u32 n = ARRAY_COUNT(LOAD_DELAYS);
    f64 lat[ARRAY_COUNT(LOAD_DELAYS)] = {}, bw[ARRAY_COUNT(LOAD_DELAYS)] = {};
    live_phase(live, "Latency under increasing load", "ns");
    u32 done = 0;
    for (; done < n && !cancel->load(); ++done) {
        j.delay = LOAD_DELAYS[done];
        j.measure.store(false);
        j.stop.store(false);
        pool_run(1 + gens, load_task, &j);
        f64 total = 0.0;
        for (u32 i = 1; i <= gens; ++i)
            if (j.slots[i].t1 > j.slots[i].t0) total += j.slots[i].bytes / (j.slots[i].t1 - j.slots[i].t0);
        lat[done] = j.ns;
        bw[done] = total / 1e9;
        live_sample(live, j.ns);
        live_progress(live, (f32)(done + 1) / n);
    }
    if (cancel->load()) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Load delay per 256 B", "cycles");
    for (u32 i = 0; i < n; ++i) {
        res_add(out, "Latency", "ns", lat[i], LOAD_DELAYS[i]);
        res_add(out, "Bandwidth", "GB/s", bw[i], LOAD_DELAYS[i]);
    }
}

// ---- read / write mix ----------------------------------------------------------------------------------
// All threads, DRAM-sized private regions. Counts bytes read + bytes written (like mem.bw.copy).

struct MixCtx {
    u8* base;
    u64 region;     // per thread
    u32 reads;      // 0 = read only
};

static f64 kern_mix(void* ctx, u32 index, u32) {
    const MixCtx* c = (const MixCtx*)ctx;
    u8* base = c->base + (u64)index * c->region;
    if (c->reads == 0) {
        k_mem_read(base, c->region);
        return (f64)c->region;
    }
    const u64 dst_bytes = c->region / (c->reads + 1) / 4096 * 4096;
    k_mem_mix(base + c->reads * dst_bytes, base, dst_bytes, c->reads);
    return (f64)(dst_bytes * (c->reads + 1));
}

void mem_bw_mix(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    MemState* m = (MemState*)state;
    const u32 threads = thread_count_for(p->thread_count);
    const u64 total = m->bytes < GiB ? m->bytes : GiB;
    struct Phase { const char* name; u32 reads; };
    static const Phase ph[] = { { "Read only", 0 }, { "3:1 read:write", 3 }, { "2:1 read:write", 2 }, { "1:1 read:write", 1 } };
    const f64 seconds = mem_point_seconds(p, 2.0);
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < ARRAY_COUNT(ph) && !cancel->load(); ++i) {
        MixCtx c = { m->arena, total / threads / 4096 * 4096, ph[i].reads };
        live_phase(live, ph[i].name, "GB/s");
        const f64 rate = run_throughput(kern_mix, &c, threads, seconds, 1e9, live, cancel, (f32)i / ARRAY_COUNT(ph),
                                        (f32)(i + 1) / ARRAY_COUNT(ph));
        res_add(out, ph[i].name, "GB/s", rate);
    }
}

// ---- bandwidth vs thread count ---------------------------------------------------------------------------
// 1, 2, ... one thread per physical core, then all logical processors. DRAM read and non-temporal write.

struct ScaleCtx {
    u8* base;
    u64 region;     // per thread
    bool write;
};

static f64 kern_scale(void* ctx, u32 index, u32) {
    const ScaleCtx* c = (const ScaleCtx*)ctx;
    u8* p = c->base + (u64)index * c->region;
    if (c->write) k_mem_write_nt(p, c->region);
    else k_mem_read(p, c->region);
    return (f64)c->region;
}

void mem_bw_scaling(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    MemState* m = (MemState*)state;
    const u32 max_threads = thread_count_for(p->thread_count);
    u32 counts[MAX_LP], n = 0;
    for (u32 t = 1; t <= max_threads && t <= g_topo.core_count; ++t) counts[n++] = t;
    if (max_threads > g_topo.core_count) counts[n++] = max_threads;
    const u64 total = m->bytes < GiB ? m->bytes : GiB;
    const f64 seconds = mem_point_seconds(p, 8.0);
    f64 rd[MAX_LP] = {}, wr[MAX_LP] = {};
    for (u32 i = 0; i < 2 * n && !cancel->load(); ++i) {
        const bool write = i >= n;
        const u32 t = counts[write ? i - n : i];
        ScaleCtx c = { m->arena, total / t / 4096 * 4096, write };
        live_phase(live, write ? "Non-temporal write" : "Read", "GB/s");
        const f64 rate = run_throughput(kern_scale, &c, t, seconds, 1e9, live, cancel, (f32)i / (2 * n),
                                        (f32)(i + 1) / (2 * n));
        (write ? wr : rd)[write ? i - n : i] = rate;
    }
    if (cancel->load()) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Threads", "threads");
    for (u32 i = 0; i < n; ++i) {
        res_add(out, "Read", "GB/s", rd[i], counts[i]);
        res_add(out, "Write (non-temporal)", "GB/s", wr[i], counts[i]);
    }
}

// ---- 4 KiB vs 2 MiB pages -----------------------------------------------------------------------------
// Pointer-chase latency for the same sizes in the 4 KiB-page arena and a large-page buffer. Large pages need
// the "Lock pages in memory" right (SeLockMemoryPrivilege), which no account has by default, not even admins.

struct PagesJob {
    u8* small;
    u8* large;
    u32* perm;
    u64 sizes[16];
    u32 count;
    f64 seconds;
    BenchLive* live;
    const CancelFlag* cancel;
    f64 ns[2][16];
};

static void pages_task(u32, u32, void* user) {
    PagesJob* j = (PagesJob*)user;
    u64 rng = 0x2545F4914F6CDD1Dull;
    for (u32 s = 0; s < j->count && !j->cancel->load(); ++s)
        for (u32 k = 0; k < 2 && !j->cancel->load(); ++k) {
            u8* buf = k ? j->large : j->small;
            chase_build(buf, j->sizes[s], j->perm, &rng);
            j->ns[k][s] = chase_ns(buf, j->sizes[s], j->seconds, j->cancel);
            live_sample(j->live, j->ns[k][s]);
            live_progress(j->live, (f32)(2 * s + k + 1) / (2 * j->count));
        }
}

void mem_lat_pages(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    MemState* m = (MemState*)state;
    const u64 max_bytes = m->bytes / 2 < GiB ? m->bytes / 2 : GiB;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    if (ms.ullAvailPhys / 4 < max_bytes) { res_skip(out, "not enough free RAM for a second buffer"); return; }
    u8* large = (u8*)big_alloc_large(max_bytes);
    if (!large) {
        res_skip(out, "2 MiB pages unavailable: needs the \"Lock pages in memory\" right (SeLockMemoryPrivilege)");
        return;
    }
    static PagesJob j;
    memset(&j, 0, sizeof(j));
    j.small = m->arena;
    j.large = large;
    j.seconds = mem_point_seconds(p, 12.0);
    j.live = live;
    j.cancel = cancel;
    for (u64 sz = 1 * MiB; sz <= max_bytes && j.count < ARRAY_COUNT(j.sizes); sz *= 4) j.sizes[j.count++] = sz;
    j.perm = (u32*)big_alloc(max_bytes / 64 * sizeof(u32));
    if (!j.perm) { big_free(large); res_fail(out, "out of memory"); return; }
    live_phase(live, "Pointer chase, 4 KiB and 2 MiB pages", "ns");
    pool_run(1, pages_task, &j);
    big_free(j.perm);
    big_free(large);
    if (cancel->load()) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Working set", "bytes");
    for (u32 s = 0; s < j.count; ++s) {
        res_add(out, "4 KiB pages", "ns", j.ns[0][s], (f64)j.sizes[s]);
        res_add(out, "2 MiB pages", "ns", j.ns[1][s], (f64)j.sizes[s]);
    }
}
