#include "core/runner.h"
#include "core/baseline.h"
#include "core/monitor.h"
#include "core/results.h"
#include "core/perf.h"
#include "core/timer.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

RunState g_run;
BenchLive g_live;
static HANDLE s_thread;

void live_reset(BenchLive* live) {
    live->progress.store(0.0f, std::memory_order_relaxed);
    live->current.store(0.0, std::memory_order_relaxed);
    live->unit.store("", std::memory_order_relaxed);
    live->phase.store("", std::memory_order_relaxed);
    for (u32 i = 0; i < LIVE_HISTORY; ++i) live->history[i].store(0.0f, std::memory_order_relaxed);
    live->history_count.store(0, std::memory_order_release);
}

void live_sample(BenchLive* live, f64 value) {
    live->current.store(value, std::memory_order_relaxed);
    u32 n = live->history_count.load(std::memory_order_relaxed);
    live->history[n % LIVE_HISTORY].store((f32)value, std::memory_order_relaxed);
    live->history_count.store(n + 1, std::memory_order_release);
}

static void finish(u32 id, BenchStatus st) {
    monitor_test_end((i32)id);      // conditions are ready before the result shows as finished
    g_status[id].store(st, std::memory_order_release);
    g_run.done_count.fetch_add(1, std::memory_order_release);
}

// Repeats: the test runs g_run.reps times in a row. The first run's result stays in place (names, units, x)
// with each value replaced by the median over the runs; g_spread gets min / max / CV. Later runs append to the
// metric arena, their values are copied out and the arena is rewound.
// Sustained mode is the same loop driven by time (g_run.sustain_s, at least two passes), and it also keeps
// a per-pass series of the tracked metrics in g_sustain.
constexpr u32 REP_MAX_METRICS = 4096;
constexpr u32 REP_VALUES = 512 * 1024;               // pass-major [pass * stride + k]
static f64 s_rep_values[REP_VALUES];
static f32 s_pass_end[MAX_SUSTAIN_PASSES];

SustainRecord g_sustain[MAX_BENCHES];
constexpr u32 SUSTAIN_POOL = 1024 * 1024;
f32 g_sustain_pool[SUSTAIN_POOL];
static u32 s_sustain_used;

static f64 median_sorted(const f64* v, u32 n) { return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]); }

static void sort_f64(f64* v, u32 n) {           // insertion sort: n <= MAX_SUSTAIN_PASSES, once per metric
    for (u32 i = 1; i < n; ++i) {
        const f64 x = v[i];
        u32 j = i;
        for (; j > 0 && v[j - 1] > x; --j) v[j] = v[j - 1];
        v[j] = x;
    }
}

static void reduce_repeats(BenchResult* r, u32 runs, u32 stride) {
    Metric* m = &g_metrics[r->first];
    MetricSpread* sp = &g_spread[r->first];
    for (u32 k = 0; k < stride; ++k) {
        static f64 v[MAX_SUSTAIN_PASSES];
        f64 sum = 0.0;
        for (u32 i = 0; i < runs; ++i) { v[i] = s_rep_values[i * stride + k]; sum += v[i]; }
        sort_f64(v, runs);
        const f64 mean = sum / runs;
        f64 var = 0.0;
        for (u32 i = 0; i < runs; ++i) var += (v[i] - mean) * (v[i] - mean);
        const f64 sd = sqrt(var / (runs - 1));
        m[k].value = median_sorted(v, runs);
        sp[k] = { (f32)v[0], (f32)v[runs - 1], mean != 0.0 ? (f32)(fabs(sd / mean) * 100.0) : 0.0f, runs };
    }
}

// Copies the per-pass values of the tracked metrics into g_sustain_pool.
static void record_sustain(u32 id, const BenchResult* r, u32 runs, u32 stride, f64 seconds) {
    SustainRecord* s = &g_sustain[id];
    s->averaged = r->kind != RESULT_VALUES;
    s->tracked = s->averaged ? 1 : stride < SUSTAIN_TRACK ? stride : SUSTAIN_TRACK;
    if (!stride || s_sustain_used + runs * (s->tracked + 1) > SUSTAIN_POOL) return;
    s->first = s_sustain_used;
    s->passes = runs;
    s->seconds = (f32)seconds;
    f32* out = &g_sustain_pool[s->first];
    const bool square = r->kind == RESULT_MATRIX && r->rows == r->cols && r->cols;
    for (u32 i = 0; i < runs; ++i) {
        const f64* v = &s_rep_values[i * stride];
        if (!s->averaged) {
            for (u32 k = 0; k < s->tracked; ++k) *out++ = (f32)v[k];
            continue;
        }
        f64 sum = 0.0;
        u32 n = 0;
        for (u32 k = 0; k < stride; ++k)
            if (!square || k / r->cols != k % r->cols) { sum += v[k]; ++n; }
        *out++ = n ? (f32)(sum / n) : 0.0f;
    }
    for (u32 i = 0; i < runs; ++i) *out++ = s_pass_end[i];
    s_sustain_used += runs * (s->tracked + 1);
}

void sustain_stats(const SustainRecord* s, u32 k, const char* unit, SustainStats* out) {
    static f64 v[MAX_SUSTAIN_PASSES];
    *out = {};
    if (!s->passes || k >= s->tracked) return;
    for (u32 i = 0; i < s->passes; ++i) v[i] = sustain_value(s, i, k);
    out->first = v[0];
    out->last = v[s->passes - 1];
    sort_f64(v, s->passes);
    out->min = v[0];
    out->max = v[s->passes - 1];
    out->median = median_sorted(v, s->passes);
    if (out->first != 0.0) {
        const f64 change = (out->last - out->first) / fabs(out->first) * 100.0;
        out->drop_pct = unit_lower_is_better(unit) ? change : -change;
    }
}

const Metric* sustain_metric(u32 bench, u32 k) {
    const SustainRecord* s = &g_sustain[bench];
    return &g_metrics[g_results[bench].first + (s->averaged ? 0 : k)];
}

static void run_test(u32 id, const BenchDesc* b, void* state, const BenchParams* params, BenchResult* r) {
    const bool sustain = g_run.sustain_s > 0;
    const f64 t0 = timer_now();
    u32 runs = 0, stride = 0;
    const u32 max_passes = sustain ? MAX_SUSTAIN_PASSES : g_run.reps;
    for (u32 rep = 0; rep < max_passes; ++rep) {
        if (sustain && rep >= 2 && timer_now() - t0 >= g_run.sustain_s) break;
        if (rep && (rep + 1) * stride > REP_VALUES) break;      // huge results: as many passes as fit
        g_run.rep.store(rep, std::memory_order_relaxed);
        if (rep) live_progress(&g_live, 0.0f);
        BenchResult extra = {};
        BenchResult* dst = rep == 0 ? r : &extra;
        const u32 mark = res_mark();
        b->fn(state, params, &g_live, dst, &g_run.cancel);
        const bool ok = !g_run.cancel.load(std::memory_order_relaxed) && !dst->error[0] && !dst->skipped;
        if (ok && rep && (extra.count != r->count || extra.kind != r->kind))
            res_fail(r, "run %u returned %u metrics instead of %u", rep + 1, extra.count, r->count);
        else if (ok) {
            if (rep == 0) stride = r->count < REP_MAX_METRICS ? r->count : REP_MAX_METRICS;
            const Metric* m = res_metrics(dst);
            for (u32 k = 0; k < stride; ++k) s_rep_values[rep * stride + k] = m[k].value;
            s_pass_end[rep] = (f32)(timer_now() - t0);
            ++runs;
        } else if (rep) {
            memcpy(r->error, extra.error, sizeof(r->error));     // a later run failing fails the test
            r->skipped = extra.skipped;
        }
        if (rep) res_rewind(mark);
        if (!ok || r->error[0]) break;
    }
    if (runs >= 2 && !r->error[0]) {
        if (sustain) record_sustain(id, r, runs, stride, timer_now() - t0);
        reduce_repeats(r, runs, stride);
    }
}

static DWORD WINAPI worker_main(void*) {
    perf_begin_run();
    perf_setup_worker_thread();
    monitor_begin(g_run.params.gpu_adapter);

    const BenchParams* params = &g_run.params;
    SuiteId suite = SUITE_COUNT;
    void* suite_state = nullptr;
    bool suite_ok = false;
    char suite_error[128] = {};

    for (u32 i = 0; i < g_run.total; ++i) {
        const u32 id = g_run.list[i];
        const BenchDesc* b = &g_benches[id];
        BenchResult* r = &g_results[id];

        if (g_run.cancel.load(std::memory_order_relaxed)) { finish(id, ST_CANCELLED); continue; }

        if (b->suite != suite) {
            if (suite != SUITE_COUNT && suite_ok && g_suites[suite].shutdown) g_suites[suite].shutdown(suite_state);
            suite = b->suite;
            suite_state = nullptr;
            suite_error[0] = 0;
            g_run.current.store((i32)id, std::memory_order_relaxed);
            live_reset(&g_live);
            live_phase(&g_live, "Suite setup", "");
            g_status[id].store(ST_RUNNING, std::memory_order_release);
            suite_ok = !g_suites[suite].init || g_suites[suite].init(params, &suite_state, suite_error, sizeof(suite_error));
        }
        if (!suite_ok) {
            snprintf(r->error, sizeof(r->error), "Suite setup failed: %s", suite_error);
            finish(id, ST_FAILED);
            continue;
        }

        live_reset(&g_live);
        g_run.current.store((i32)id, std::memory_order_relaxed);
        g_status[id].store(ST_RUNNING, std::memory_order_release);

        const f64 t0 = timer_now();
        g_run.test_start.store(t0, std::memory_order_relaxed);
        run_test(id, b, suite_state, params, r);
        r->seconds = timer_now() - t0;

        BenchStatus st = ST_DONE;
        if (g_run.cancel.load(std::memory_order_relaxed)) st = ST_CANCELLED;
        else if (r->skipped) st = ST_SKIPPED;
        else if (r->error[0]) st = ST_FAILED;
        finish(id, st);
    }
    if (suite != SUITE_COUNT && suite_ok && g_suites[suite].shutdown) g_suites[suite].shutdown(suite_state);

    monitor_end();
    g_run.current.store(-1, std::memory_order_relaxed);
    perf_end_run();
    g_run.end_time = timer_now();
    g_run.running.store(false, std::memory_order_release);
    return 0;
}

void runner_init() {
    g_run.current.store(-1);
    for (u32 i = 0; i < g_bench_count; ++i) g_status[i].store(ST_NOT_RUN);
}

bool runner_start(const bool* selected, const BenchParams* params) {
    if (g_run.running.load(std::memory_order_acquire)) return false;
    if (s_thread) { WaitForSingleObject(s_thread, INFINITE); CloseHandle(s_thread); s_thread = nullptr; }

    results_reset();
    // Run order: by suite, then registry order, so each suite's init/shutdown happens once.
    u32 n = 0;
    for (u32 s = 0; s < SUITE_COUNT; ++s)
        for (u32 i = 0; i < g_bench_count; ++i)
            if (g_benches[i].suite == s && selected[i]) g_run.list[n++] = i;
    for (u32 i = 0; i < g_bench_count; ++i) g_status[i].store(selected[i] ? ST_PENDING : ST_NOT_RUN);
    if (n == 0) return false;

    g_run.total = n;
    g_run.params = *params;
    g_run.reps = params->repeat < 1 ? 1 : params->repeat > MAX_REPEAT ? MAX_REPEAT : params->repeat;
    g_run.sustain_s = params->sustain_s > MAX_SUSTAIN_S ? MAX_SUSTAIN_S : params->sustain_s;
    if (g_run.sustain_s) g_run.reps = 1;
    memset(g_sustain, 0, sizeof(g_sustain));
    s_sustain_used = 0;
    g_run.rep.store(0);
    g_run.cancel.store(false);
    g_run.done_count.store(0);
    g_run.current.store(-1);
    g_run.start_time = timer_now();
    g_run.end_time = 0.0;
    live_reset(&g_live);
    g_run.running.store(true, std::memory_order_release);

    s_thread = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
    if (!s_thread) { g_run.running.store(false); return false; }
    return true;
}

void runner_cancel() { g_run.cancel.store(true, std::memory_order_relaxed); }

bool runner_is_running() { return g_run.running.load(std::memory_order_acquire); }

void runner_wait() {
    if (!s_thread) return;
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    s_thread = nullptr;
}

f32 runner_overall_progress() {
    if (g_run.total == 0) return 0.0f;
    f32 done = (f32)g_run.done_count.load(std::memory_order_acquire);
    if (g_run.current.load(std::memory_order_relaxed) >= 0 && done < g_run.total) {
        if (g_run.sustain_s) {
            const f64 t = (timer_now() - g_run.test_start.load(std::memory_order_relaxed)) / g_run.sustain_s;
            done += t < 0.99 ? (f32)t : 0.99f;
        } else {
            done += (g_run.rep.load(std::memory_order_relaxed) + g_live.progress.load(std::memory_order_relaxed)) / g_run.reps;
        }
    }
    f32 p = done / (f32)g_run.total;
    return p > 1.0f ? 1.0f : p;
}
