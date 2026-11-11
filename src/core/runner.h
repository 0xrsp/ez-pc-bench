#pragma once
// Worker thread that runs the selected benchmarks one by one.
#include "core/bench.h"

struct RunState {
    std::atomic<bool> running;
    std::atomic<bool> cancel;
    std::atomic<i32>  current;        // bench id being run, -1 if none
    std::atomic<u32>  done_count;     // finished tests in this run
    std::atomic<u32>  rep;            // repetition of the current test (0-based); reps = params.repeat clamped
    u32 total;                        // tests in this run
    u32 list[MAX_BENCHES];            // bench ids in run order
    BenchParams params;               // snapshot taken at start
    u32 reps;                         // repetitions per test (1..MAX_REPEAT)
    f64 start_time, end_time;
};

extern RunState g_run;
extern BenchLive g_live;

void runner_init();
bool runner_start(const bool* selected, const BenchParams* params);   // selected: g_bench_count bools
void runner_cancel();
bool runner_is_running();
void runner_wait();     // blocks until the worker exits

// 0..1 over the whole run (finished tests + fraction of the current one).
f32 runner_overall_progress();
