#pragma once
// Multi-threaded throughput driver shared by the CPU and memory suites.
// Pool threads call `kernel` in a loop until stopped; each thread measures its own rate after a
// short warm-up and the rates are summed. Live samples use a ~0.5 s sliding window.
#include "core/bench.h"

typedef std::atomic<bool> CancelFlag;
typedef f64 (*KernelFn)(void* ctx, u32 index, u32 count);   // one call = a fixed chunk; returns work done

// Returns the total rate (work units per second) divided by `unit_div`.
f64 run_throughput(KernelFn kernel, void* ctx, u32 threads, f64 seconds, f64 unit_div, BenchLive* live,
                   const CancelFlag* cancel, f32 progress_from, f32 progress_to);

// Standard ST + MT test with metrics "Single thread", "Multi-thread", "MT / ST".
void st_mt_test(KernelFn kernel, void* ctx, const char* unit, f64 unit_div, const BenchParams* p,
                BenchLive* live, BenchResult* out, const CancelFlag* cancel);
