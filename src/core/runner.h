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
    u32 reps;                         // repetitions per test (1..MAX_REPEAT); 1 in sustained mode
    u32 sustain_s;                    // sustained mode: seconds per test (params.sustain_s clamped), 0 = off
    std::atomic<f64> test_start;      // timer_now() when the current test started (sustained-mode progress)
    f64 start_time, end_time;
};

extern RunState g_run;
extern BenchLive g_live;

// Sustained mode: per-pass values of each test, for the per-pass chart and the TOON `sustained` tables.
// Values results track their first SUSTAIN_TRACK metrics; series / matrix results track one value per pass,
// the average over all points (matrix: off-diagonal cells), named after the first metric.
constexpr u32 SUSTAIN_TRACK = 16;
constexpr u32 MAX_SUSTAIN_PASSES = 500;
struct SustainRecord {
    u32 passes;                       // 0 = the test didn't run in sustained mode
    u32 tracked;                      // metrics tracked
    bool averaged;                    // series / matrix: the one tracked value is the average over all points
    f32 seconds;                      // wall time of all passes
    u32 first;                        // g_sustain_pool: passes * tracked values (pass-major), then `passes` end times (s)
};
extern SustainRecord g_sustain[MAX_BENCHES];
extern f32 g_sustain_pool[];

struct SustainStats { f64 first, last, min, max, median, drop_pct; };
inline f32 sustain_value(const SustainRecord* s, u32 pass, u32 k) { return g_sustain_pool[s->first + pass * s->tracked + k]; }
inline f32 sustain_time(const SustainRecord* s, u32 pass) { return g_sustain_pool[s->first + s->passes * s->tracked + pass]; }
// First / last / min / max / median of tracked metric k over the passes; drop_pct = how much worse the last pass
// is than the first (positive = slower at the end; respects lower-is-better units).
void sustain_stats(const SustainRecord* s, u32 k, const char* unit, SustainStats* out);
// Tracked metric k's name / unit (from the result's metrics).
const Metric* sustain_metric(u32 bench, u32 k);

void runner_init();
bool runner_start(const bool* selected, const BenchParams* params);   // selected: g_bench_count bools
void runner_cancel();
bool runner_is_running();
void runner_wait();     // blocks until the worker exits

// 0..1 over the whole run (finished tests + fraction of the current one).
f32 runner_overall_progress();
