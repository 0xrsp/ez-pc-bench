#pragma once
// Shared between the stability controller (stability.cpp) and the test files (stab_*.cpp).
#include "stability/stability.h"

// Records one error: counts it for the test (and LP / chunk when >= 0), logs it among the first STAB_MAX_LOG,
// and stops the run when stop-on-first-error is set.
void stab_error(StabTest test, i32 lp, i32 chunk, u64 address, const char* what, u64 expected, u64 actual);
// Counts `n` errors without log entries (GPU tests: more mismatches than the GPU-side log holds).
void stab_add_errors(StabTest test, i32 chunk, u64 n);
// True when the test should end: the run was stopped or its deadline (timer_now() seconds) passed.
bool stab_done(f64 deadline);
// Sets a test's current phase (static string).
inline void stab_phase(StabTest t, const char* phase) { g_stab.tests[t].phase.store(phase, std::memory_order_relaxed); }
inline void stab_work(StabTest t, f64 add) {
    std::atomic<f64>& w = g_stab.tests[t].work;
    w.store(w.load(std::memory_order_relaxed) + add, std::memory_order_relaxed);   // single writer per call site
}
// Atomic add for work counters written by several threads.
void stab_work_mt(StabTest t, f64 add);

// Runs fn(index, user) on `count` new threads pinned to lps[index] (normal priority, power throttling off) and
// waits for all of them.
typedef void (*StabThreadFn)(u32 index, void* user);
void stab_run_threads(u32 count, const u32* lps, StabThreadFn fn, void* user);

// SplitMix64: seeds and pattern streams.
inline u64 stab_mix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Test bodies. Each runs until stab_done(deadline), sets status / note / size in g_stab.tests[test] and returns.
// The controller sets SS_RUNNING before and resolves pass / fail / stopped after (unless the body set SS_ERROR).
void stab_cpu_run(f64 deadline);
void stab_cache_run(f64 deadline);
void stab_cores_run(f64 deadline);
void stab_mem_run(f64 deadline);
void stab_disk_run(f64 deadline);
void stab_gpu_run(StabTest test, f64 deadline);     // STAB_VRAM, STAB_GPU, STAB_POWER (stab_gpu.cpp)
// Sets SS_ERROR with a note (the test couldn't run).
void stab_fail_setup(StabTest test, const char* fmt, ...);
