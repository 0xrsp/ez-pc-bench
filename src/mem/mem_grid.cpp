// Cache & memory grid, AIDA64 style: read / write / copy bandwidth with all threads and single-thread latency
// for L1, L2, L3 and DRAM, each measured at a working-set size that sits inside that level.
//
// Bandwidth: every thread works on its own region. Per-thread size: L1 = half the L1D, L2 = half the L2 (both
// divided by the threads sharing a core), L3 = 3/4 of all L3 split over the threads, DRAM = up to 1 GiB split
// over the threads. Copy moves the first half of the region to the second half and counts both (bytes read +
// written, like mem.bw.copy). Latency: random pointer chase on the best core over half the L1D / half the L2 /
// half of one L3 (at least 2x the L2) / 256 MiB, 4 KiB pages.
#include "mem/mem_internal.h"
#include "mem/mem_kernels.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include "core/threads.h"
#include <string.h>

enum GridOp : u8 { GRID_READ, GRID_WRITE, GRID_COPY };

struct GridCtx {
    u8* base;
    u64 size;       // per-thread working set
    u64 stride;     // distance between per-thread regions
    GridOp op;
};

static f64 kern_grid(void* ctx, u32 index, u32) {
    const GridCtx* c = (const GridCtx*)ctx;
    u8* p = c->base + (u64)index * c->stride;
    const u64 reps = c->size >= 4 * MiB ? 1 : 4 * MiB / c->size;
    for (u64 r = 0; r < reps; ++r) {
        switch (c->op) {
            case GRID_READ: k_mem_read(p, c->size); break;
            case GRID_WRITE: k_mem_write(p, c->size); break;
            case GRID_COPY: k_mem_copy(p + c->size / 2, p, c->size / 2); break;
        }
    }
    return (f64)(reps * c->size);
}

struct GridLatJob {
    u8* buf;
    u64 bytes;
    u32* perm;
    f64 seconds;
    const CancelFlag* cancel;
    f64 ns;
};

static void grid_latency_task(u32, u32, void* user) {
    GridLatJob* j = (GridLatJob*)user;
    u64 rng = 0x9E3779B97F4A7C15ull;
    chase_build(j->buf, j->bytes, j->perm, &rng);
    j->ns = chase_ns(j->buf, j->bytes, j->seconds, j->cancel);
}

static u64 round_4k(u64 v) { return v < 4096 ? 4096 : v / 4096 * 4096; }

// Size of one L3 instance (one CCD / CCX); falls back to the total.
static u64 l3_instance_bytes() {
    for (u32 i = 0; i < g_cpux.cache_count; ++i)
        if (g_cpux.caches[i].level == 3) return (u64)g_cpux.caches[i].size_kib * KiB;
    return (u64)g_sys.cpu.l3_kib * KiB;
}

static const char* const GRID_NAMES[4][4] = {
    { "L1 read", "L1 write", "L1 copy", "L1 latency" },
    { "L2 read", "L2 write", "L2 copy", "L2 latency" },
    { "L3 read", "L3 write", "L3 copy", "L3 latency" },
    { "Memory read", "Memory write", "Memory copy", "Memory latency" },
};

void mem_grid(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    MemState* m = (MemState*)state;
    const CpuInfo* c = &g_sys.cpu;
    if (!c->l1d_kib || !c->l2_kib) { res_fail(out, "cache sizes unknown"); return; }
    const u32 threads = thread_count_for(p->thread_count);
    const u32 cores = g_topo.core_count ? g_topo.core_count : 1;
    const u32 per_core = threads > cores ? (threads + cores - 1) / cores : 1;
    const u64 l1 = (u64)c->l1d_kib * KiB, l2 = (u64)c->l2_kib * KiB, l3_all = (u64)c->l3_kib * KiB;
    const u64 l3 = l3_instance_bytes();
    const u64 dram = m->bytes < GiB ? m->bytes : GiB;

    u64 bw[4] = { round_4k(l1 / 2 / per_core), round_4k(l2 / 2 / per_core), round_4k(l3_all * 3 / 4 / threads),
                  round_4k(dram / threads) };
    u64 lat_l3 = l3 / 2 > 2 * l2 ? l3 / 2 : 2 * l2;
    if (lat_l3 > l3 * 3 / 4) lat_l3 = l3 * 3 / 4;
    u64 lat[4] = { round_4k(l1 / 2), round_4k(l2 / 2), round_4k(lat_l3), 256 * MiB < m->bytes ? 256 * MiB : m->bytes };
    const bool have_l3 = l3_all > 0;

    u32* perm = (u32*)big_alloc(lat[3] / 64 * sizeof(u32));
    if (!perm) { res_fail(out, "out of memory"); return; }

    const f64 seconds = mem_point_seconds(p, 4.0);
    const u32 levels = 4, total = (have_l3 ? 4 : 3) * 4;
    u32 done = 0;
    f64 values[4][4] = {};
    for (u32 lv = 0; lv < levels && !cancel->load(); ++lv) {
        if (lv == 2 && !have_l3) continue;
        for (u32 op = 0; op < 3 && !cancel->load(); ++op) {
            GridCtx g = { m->arena, bw[lv], bw[lv], (GridOp)op };
            live_phase(live, GRID_NAMES[lv][op], "GB/s");
            values[lv][op] = run_throughput(kern_grid, &g, threads, seconds, 1e9, live, cancel, (f32)done / total,
                                            (f32)(done + 1) / total);
            ++done;
        }
        if (cancel->load()) break;
        live_phase(live, GRID_NAMES[lv][3], "ns");
        GridLatJob j = { m->arena, lat[lv], perm, seconds, cancel, 0.0 };
        pool_run(1, grid_latency_task, &j);     // pool worker 0 = best core
        values[lv][3] = j.ns;
        live_sample(live, j.ns);
        live_progress(live, (f32)++done / total);
    }
    big_free(perm);
    if (cancel->load()) return;

    res_begin(out, RESULT_VALUES);
    static const char* const units[4] = { "GB/s", "GB/s", "GB/s", "ns" };
    for (u32 lv = 0; lv < levels; ++lv) {
        if (lv == 2 && !have_l3) continue;
        for (u32 k = 0; k < 4; ++k) res_add(out, GRID_NAMES[lv][k], units[k], values[lv][k]);
    }
}
