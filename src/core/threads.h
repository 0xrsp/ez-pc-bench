#pragma once
// CPU topology + a small pool of pinned worker threads.
#include "core/types.h"
#include <atomic>

constexpr u32 MAX_LP = 64;

struct CpuTopology {
    u32 lp_count;
    u32 core_count;
    u16 group[MAX_LP];      // per LP (our index): processor group
    u8  number[MAX_LP];     // per LP: number within group
    u16 core[MAX_LP];       // per LP: physical core id (0..core_count-1)
    u8  sched_class[MAX_LP];// per LP: Windows scheduling class (higher = preferred core)
    u8  eff_class[MAX_LP];  // per LP: CPU-set EfficiencyClass (hybrid CPUs: higher = performance core; else all 0)
    u32 order[MAX_LP];      // LPs in placement order: best physical core first, one LP per core, then SMT siblings
    u32 best_lp;            // = order[0]
};

extern CpuTopology g_topo;

void topo_init();
void pin_current_thread(u32 lp);

// Pool of g_topo.lp_count workers; worker i is pinned to g_topo.order[i].
typedef void (*PoolFn)(u32 index, u32 count, void* user);

void pool_init();       // idempotent, threads live until process exit
void pool_dispatch(u32 count, PoolFn fn, void* user);   // run fn on workers 0..count-1, returns immediately
void pool_wait();       // wait for the current dispatch to finish
inline void pool_run(u32 count, PoolFn fn, void* user) { pool_dispatch(count, fn, user); pool_wait(); }

// Resolve a requested thread count (0 = all LPs) against the topology.
u32 thread_count_for(u32 requested);
