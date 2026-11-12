// Per-core single-thread pass and the hybrid (P-core / E-core) split.
#include "cpu/cpu_internal.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/throughput.h"
#include <string.h>

// Runs `kernel` with worker i pinned to lps[i] instead of the pool's default placement. Workers re-pin
// themselves on their first call; the pool restores its own pinning after the dispatch.
struct PinnedKernel {
    KernelFn kernel;
    void* ctx;
    u32 lps[MAX_LP];
    bool pinned[MAX_LP];
};

static f64 kern_pinned(void* c, u32 index, u32 count) {
    PinnedKernel* p = (PinnedKernel*)c;
    if (!p->pinned[index]) {
        pin_current_thread(p->lps[index]);
        p->pinned[index] = true;
    }
    return p->kernel(p->ctx, index, count);
}

static f64 run_pinned(PinnedKernel* p, u32 threads, f64 seconds, f64 unit_div, BenchLive* live, const CancelFlag* cancel,
                      f32 from, f32 to) {
    memset(p->pinned, 0, sizeof(p->pinned));
    return run_throughput(kern_pinned, p, threads, seconds, unit_div, live, cancel, from, to);
}

static constexpr u64 TOPO_ITERS = 1u << 18;
static f64 kern_int_alu(void*, u32, u32) { return (f64)k_int_alu(TOPO_ITERS); }
static f64 kern_add_chain(void*, u32, u32) { return (f64)k_add_chain(TOPO_ITERS); }

// ---- per-core pass -----------------------------------------------------------------------------------
// One thread on the first logical processor of each physical core in turn: integer ALU throughput and the
// core clock from a chain of dependent single-cycle adds. Shows preferred cores, CCD and P / E differences.

void cpu_percore(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u32 cores = g_topo.core_count;
    u32 lp_of_core[MAX_LP];
    for (u32 c = 0; c < cores; ++c) {
        lp_of_core[c] = 0;
        for (u32 lp = g_topo.lp_count; lp-- > 0;)
            if (g_topo.core[lp] == c) lp_of_core[c] = lp;       // lowest LP of the core
    }
    f64 seconds = p->duration_ms / 1000.0 / cores;
    if (seconds < 0.25) seconds = 0.25;

    static f64 ops[MAX_LP], mhz[MAX_LP];
    static PinnedKernel pk;
    for (u32 c = 0; c < cores && !cancel->load(); ++c) {
        pk.lps[0] = lp_of_core[c];
        const f32 f0 = (f32)c / cores, f1 = (f32)(c + 1) / cores;
        live_phase(live, "Integer ALU, one core at a time", "Gops/s");
        pk.kernel = kern_int_alu;
        ops[c] = run_pinned(&pk, 1, seconds, 1e9, live, cancel, f0, 0.5f * (f0 + f1));
        live_phase(live, "Core clock (dependent adds), one core at a time", "MHz");
        pk.kernel = kern_add_chain;
        mhz[c] = run_pinned(&pk, 1, seconds, 1e6, live, cancel, 0.5f * (f0 + f1), f1);
    }
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Core", "core");
    for (u32 c = 0; c < cores; ++c) res_add(out, "Int ALU", "Gops/s", ops[c], (f64)c);
    for (u32 c = 0; c < cores; ++c) res_add(out, "Clock (add chain)", "MHz", mhz[c], (f64)c);
}

// ---- hybrid split --------------------------------------------------------------------------------------
// Each efficiency class (CPU sets) on its own: one thread on its best core, then all of its logical
// processors, running the LZ77 compressor. Non-hybrid CPUs (one class) skip. Untested: written on a
// single-class Zen 4.

void cpu_hybrid(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    u8 classes[4];
    u32 class_count = 0;
    for (u32 lp = 0; lp < g_topo.lp_count; ++lp) {
        u32 k = 0;
        while (k < class_count && classes[k] != g_topo.eff_class[lp]) ++k;
        if (k == class_count && class_count < ARRAY_COUNT(classes)) classes[class_count++] = g_topo.eff_class[lp];
    }
    if (class_count < 2) {
        res_skip(out, "not a hybrid CPU (all cores have efficiency class %u)", g_topo.eff_class[0]);
        return;
    }
    for (u32 i = 1; i < class_count; ++i)       // highest class (performance cores) first
        for (u32 j = i; j > 0 && classes[j] > classes[j - 1]; --j) { u8 t = classes[j]; classes[j] = classes[j - 1]; classes[j - 1] = t; }

    void* lz = lz77_ctx_create();
    if (!lz) { res_fail(out, "out of memory"); return; }
    static PinnedKernel pk;
    pk.kernel = kern_lz77_compress;
    pk.ctx = lz;
    static const char* const ST_NAMES[] = { "P-cores, one thread", "E-cores, one thread", "LP E-cores, one thread", "Class 4, one thread" };
    static const char* const MT_NAMES[] = { "P-cores, all threads", "E-cores, all threads", "LP E-cores, all threads", "Class 4, all threads" };
    f64 st[4] = {}, mt[4] = {};
    const f64 seconds = p->duration_ms / 1000.0 / class_count;
    for (u32 k = 0; k < class_count && !cancel->load(); ++k) {
        u32 n = 0;
        for (u32 i = 0; i < g_topo.lp_count; ++i)      // placement order: best cores first, SMT siblings last
            if (g_topo.eff_class[g_topo.order[i]] == classes[k]) pk.lps[n++] = g_topo.order[i];
        const f32 f0 = (f32)k / class_count, f1 = (f32)(k + 1) / class_count;
        live_phase(live, ST_NAMES[k], "MB/s");
        st[k] = run_pinned(&pk, 1, seconds, 1e6, live, cancel, f0, 0.5f * (f0 + f1));
        live_phase(live, MT_NAMES[k], "MB/s");
        mt[k] = run_pinned(&pk, n, seconds, 1e6, live, cancel, 0.5f * (f0 + f1), f1);
    }
    const bool ok = lz77_ctx_verify(lz);
    lz77_ctx_destroy(lz);
    res_begin(out, RESULT_VALUES);
    for (u32 k = 0; k < class_count; ++k) {
        res_add(out, ST_NAMES[k], "MB/s", st[k]);
        res_add(out, MT_NAMES[k], "MB/s", mt[k]);
    }
    res_add(out, "E / P, one thread", "x", st[0] > 0.0 ? st[1] / st[0] : 0.0);
    if (!ok) res_fail(out, "LZ77 round trip failed");
}
