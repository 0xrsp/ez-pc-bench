// Smoke-test suite: exercises the runner, live stats and every result kind.
#include "core/bench.h"
#include "core/results.h"
#include "core/timer.h"
#include <windows.h>
#include <string.h>

struct SmokeState {
    u8* buf;
    u64 size;
};

static constexpr u64 SMOKE_BUF = 256 * MiB;

bool smoke_init(const BenchParams*, void** state, char* error, u32 error_size) {
    u8* buf = (u8*)VirtualAlloc(nullptr, SMOKE_BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        strncpy_s(error, error_size, "VirtualAlloc failed", _TRUNCATE);
        return false;
    }
    memset(buf, 1, SMOKE_BUF);  // pre-fault
    static SmokeState s;
    s = { buf, SMOKE_BUF };
    *state = &s;
    return true;
}

void smoke_shutdown(void* state) {
    SmokeState* s = (SmokeState*)state;
    VirtualFree(s->buf, 0, MEM_RELEASE);
}

void smoke_spin(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const std::atomic<bool>* cancel) {
    live_phase(live, "Spinning", "Mops/s");
    const f64 duration = p->duration_ms / 1000.0;
    const u32 chunk = 1u << 20;
    u64 x = 0x9E3779B97F4A7C15ull, total = 0, window_ops = 0;
    f64 peak = 0.0;
    const f64 t0 = timer_now();
    f64 window_t = t0, now = t0;
    while (now - t0 < duration && !cancel->load(std::memory_order_relaxed)) {
        for (u32 i = 0; i < chunk; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
        do_not_optimize(x);
        total += chunk;
        window_ops += chunk;
        now = timer_now();
        if (now - window_t >= 0.05) {
            f64 rate = window_ops / (now - window_t) / 1e6;
            if (rate > peak) peak = rate;
            live_sample(live, rate);
            live_progress(live, (f32)((now - t0) / duration));
            window_t = now;
            window_ops = 0;
        }
    }
    res_begin(out, RESULT_VALUES);
    res_add(out, "Average", "Mops/s", total / (now - t0) / 1e6);
    res_add(out, "Peak (50 ms window)", "Mops/s", peak);
}

void smoke_sweep(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const std::atomic<bool>* cancel) {
    SmokeState* s = (SmokeState*)state;
    live_phase(live, "Sweeping sizes", "GB/s");
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Block size", "bytes");

    const u32 steps = 15;   // 16 KiB .. 256 MiB
    const f64 step_time = p->duration_ms / 1000.0 / steps;
    for (u32 i = 0; i < steps && !cancel->load(std::memory_order_relaxed); ++i) {
        u64 size = (16 * KiB) << i;
        u64 bytes = 0;
        f64 t0 = timer_now(), now = t0;
        u8 v = (u8)i;
        do {
            memset(s->buf, v++, size);
            clobber_memory();
            bytes += size;
            now = timer_now();
        } while (now - t0 < step_time);
        f64 gbs = bytes / (now - t0) / 1e9;
        live_sample(live, gbs);
        live_progress(live, (f32)(i + 1) / steps);
        res_add(out, "Write", "GB/s", gbs, (f64)size);
    }
}

void smoke_matrix(void* state, const BenchParams* p, BenchLive* live, BenchResult* out, const std::atomic<bool>* cancel) {
    SmokeState* s = (SmokeState*)state;
    live_phase(live, "Copy src/dst offsets", "GB/s");
    res_begin(out, RESULT_VALUES);

    const u32 n = 4;            // offsets 0, 16, 32, 48 bytes
    const u64 size = 4 * MiB;   // fits in L3
    const f64 cell_time = p->duration_ms / 1000.0 / (n * n);
    for (u32 r = 0; r < n; ++r) {
        for (u32 c = 0; c < n; ++c) {
            if (cancel->load(std::memory_order_relaxed)) return;
            u8* src = s->buf + r * 16;
            u8* dst = s->buf + 64 * MiB + c * 16;
            u64 bytes = 0;
            f64 t0 = timer_now(), now = t0;
            do {
                memcpy(dst, src, size);
                clobber_memory();
                bytes += size;
                now = timer_now();
            } while (now - t0 < cell_time);
            f64 gbs = bytes / (now - t0) / 1e9;
            live_sample(live, gbs);
            live_progress(live, (f32)(r * n + c + 1) / (n * n));
            res_add(out, "Copy", "GB/s", gbs);
        }
    }
    res_matrix(out, (u16)n, (u16)n);
}
