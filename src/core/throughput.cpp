#include "core/throughput.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/timer.h"
#include <windows.h>

struct alignas(64) ThreadSlot {
    std::atomic<f64> work;      // work done since measuring started
    f64 rate;                   // final work / second
};

struct ThroughputJob {
    KernelFn kernel;
    void* ctx;
    std::atomic<bool> measure;
    std::atomic<bool> stop;
    ThreadSlot slots[MAX_LP];
};

static void throughput_worker(u32 index, u32 count, void* user) {
    ThroughputJob* job = (ThroughputJob*)user;
    ThreadSlot* slot = &job->slots[index];
    f64 work = 0.0, t0 = 0.0;
    bool measuring = false;
    while (!job->stop.load(std::memory_order_relaxed)) {
        const f64 w = job->kernel(job->ctx, index, count);
        if (measuring) {
            work += w;
            slot->work.store(work, std::memory_order_relaxed);
        } else if (job->measure.load(std::memory_order_relaxed)) {
            measuring = true;
            t0 = timer_now();
        }
    }
    const f64 t1 = timer_now();
    slot->rate = (measuring && t1 > t0) ? work / (t1 - t0) : 0.0;
}

// Returns total rate in work units per second divided by `unit_div`.
f64 run_throughput(KernelFn kernel, void* ctx, u32 threads, f64 seconds, f64 unit_div, BenchLive* live,
                   const CancelFlag* cancel, f32 progress_from, f32 progress_to) {
    static ThroughputJob job;
    job.kernel = kernel;
    job.ctx = ctx;
    job.measure.store(false);
    job.stop.store(false);
    for (u32 i = 0; i < MAX_LP; ++i) { job.slots[i].work.store(0.0); job.slots[i].rate = 0.0; }

    pool_dispatch(threads, throughput_worker, &job);
    const f64 warmup = seconds * 0.1 < 0.3 ? seconds * 0.1 : 0.3;
    const f64 t_start = timer_now();
    while (timer_now() - t_start < warmup && !cancel->load(std::memory_order_relaxed)) Sleep(5);

    job.measure.store(true);
    const f64 t0 = timer_now();
    // Live rate over a sliding ~0.5 s window, so slow kernels (one call per 0.2 s) don't spike.
    constexpr u32 WIN = 10;
    f64 win_t[WIN], win_work[WIN];
    u32 win_n = 0;
    f64 now = t0;
    while (now - t0 < seconds && !cancel->load(std::memory_order_relaxed)) {
        Sleep(50);
        now = timer_now();
        f64 sum = 0.0;
        for (u32 i = 0; i < threads; ++i) sum += job.slots[i].work.load(std::memory_order_relaxed);
        win_t[win_n % WIN] = now;
        win_work[win_n % WIN] = sum;
        ++win_n;
        const u32 oldest = win_n > WIN ? win_n % WIN : 0;
        const f64 dt = now - win_t[oldest], dw = sum - win_work[oldest];
        if (sum > 0.0 && dt > 0.0) live_sample(live, (win_n > 1 ? dw / dt : sum / (now - t0)) / unit_div);
        f64 frac = (now - t0) / seconds;
        live_progress(live, progress_from + (progress_to - progress_from) * (f32)(frac < 1.0 ? frac : 1.0));
    }
    job.stop.store(true);
    pool_wait();

    f64 total = 0.0;
    for (u32 i = 0; i < threads; ++i) total += job.slots[i].rate;
    return total / unit_div;
}

// Standard ST + MT test: "Single thread", "Multi-thread", "MT / ST".
void st_mt_test(KernelFn kernel, void* ctx, const char* unit, f64 unit_div, const BenchParams* p,
                BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const f64 seconds = p->duration_ms / 1000.0;
    const u32 mt = thread_count_for(p->thread_count);
    live_phase(live, "Single thread", unit);
    const f64 st_rate = run_throughput(kernel, ctx, 1, seconds, unit_div, live, cancel, 0.0f, 0.5f);
    live_phase(live, "Multi-thread", unit);
    const f64 mt_rate = run_throughput(kernel, ctx, mt, seconds, unit_div, live, cancel, 0.5f, 1.0f);
    res_begin(out, RESULT_VALUES);
    res_add(out, "Single thread", unit, st_rate);
    res_add(out, "Multi-thread", unit, mt_rate);
    res_add(out, "MT / ST", "x", st_rate > 0.0 ? mt_rate / st_rate : 0.0);
}
