#include "core/runner.h"
#include "core/results.h"
#include "core/perf.h"
#include "core/timer.h"
#include <windows.h>
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
    g_status[id].store(st, std::memory_order_release);
    g_run.done_count.fetch_add(1, std::memory_order_release);
}

static DWORD WINAPI worker_main(void*) {
    perf_begin_run();
    perf_setup_worker_thread();

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
        b->fn(suite_state, params, &g_live, r, &g_run.cancel);
        r->seconds = timer_now() - t0;

        BenchStatus st = ST_DONE;
        if (g_run.cancel.load(std::memory_order_relaxed)) st = ST_CANCELLED;
        else if (r->skipped) st = ST_SKIPPED;
        else if (r->error[0]) st = ST_FAILED;
        finish(id, st);
    }
    if (suite != SUITE_COUNT && suite_ok && g_suites[suite].shutdown) g_suites[suite].shutdown(suite_state);

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
    if (g_run.current.load(std::memory_order_relaxed) >= 0 && done < g_run.total)
        done += g_live.progress.load(std::memory_order_relaxed);
    f32 p = done / (f32)g_run.total;
    return p > 1.0f ? 1.0f : p;
}
