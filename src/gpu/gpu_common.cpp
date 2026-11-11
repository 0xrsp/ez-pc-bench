// Shared GPU-suite helpers: preview hand-off to the UI, frame-time statistics, compute reporting.
#include "gpu/gpu_common.h"
#include "core/results.h"
#include <windows.h>
#include <algorithm>
#include <string.h>

// ---- preview -------------------------------------------------------------------------------------

struct Preview {
    SRWLOCK lock;
    u32 seq;            // bumped on every publish / clear
    u32 w, h;           // 0 = no preview active
    u8 rgba[GPU_PREVIEW_MAX_W * GPU_PREVIEW_MAX_H * 4];
};
static Preview s_preview = { SRWLOCK_INIT };

void gpu_preview_size(u32 rt_w, u32 rt_h, u32* w, u32* h) {
    if (rt_w == 0 || rt_h == 0) { *w = *h = 0; return; }
    // Fit inside MAX keeping aspect; width rounded down to 64 px (256-byte rows for D3D12 readback).
    f64 s = (f64)GPU_PREVIEW_MAX_W / rt_w;
    if (rt_h * s > GPU_PREVIEW_MAX_H) s = (f64)GPU_PREVIEW_MAX_H / rt_h;
    u32 pw = (u32)(rt_w * s) / 64 * 64;
    if (pw < 64) pw = 64;
    u32 ph = (u32)((f64)pw * rt_h / rt_w + 0.5);
    if (ph < 1) ph = 1;
    if (ph > GPU_PREVIEW_MAX_H) ph = GPU_PREVIEW_MAX_H;
    *w = pw;
    *h = ph;
}

void gpu_preview_publish(const u8* rgba, u32 w, u32 h, u32 row_pitch) {
    if (w > GPU_PREVIEW_MAX_W || h > GPU_PREVIEW_MAX_H || !rgba) return;
    AcquireSRWLockExclusive(&s_preview.lock);
    for (u32 y = 0; y < h; ++y) memcpy(s_preview.rgba + (u64)y * w * 4, rgba + (u64)y * row_pitch, (u64)w * 4);
    s_preview.w = w;
    s_preview.h = h;
    s_preview.seq++;
    ReleaseSRWLockExclusive(&s_preview.lock);
}

void gpu_preview_clear() {
    AcquireSRWLockExclusive(&s_preview.lock);
    s_preview.w = s_preview.h = 0;
    s_preview.seq++;
    ReleaseSRWLockExclusive(&s_preview.lock);
}

bool gpu_preview_acquire(u32* seq, u8* dst, u32* w, u32* h) {
    AcquireSRWLockShared(&s_preview.lock);
    const bool newer = s_preview.seq != *seq;
    if (newer) {
        *seq = s_preview.seq;
        *w = s_preview.w;
        *h = s_preview.h;
        if (dst && s_preview.w) memcpy(dst, s_preview.rgba, (u64)s_preview.w * s_preview.h * 4);
    }
    ReleaseSRWLockShared(&s_preview.lock);
    return newer;
}

// ---- adapter / timing -------------------------------------------------------------------------------

const GpuInfo* gpu_selected(const BenchParams* p) {
    if (g_sys.gpu_count == 0) return nullptr;
    return &g_sys.gpus[p->gpu_adapter < g_sys.gpu_count ? p->gpu_adapter : 0];
}

f64 gpu_warmup_seconds(const BenchParams* p) {
    const f64 third = p->duration_ms / 3000.0;
    return third < 1.0 ? third : 1.0;
}

// ---- frame statistics ----------------------------------------------------------------------------------

constexpr u32 GPU_MAX_FRAMES = 1u << 20;
static f32 s_frames[GPU_MAX_FRAMES];
static f32 s_sorted[GPU_MAX_FRAMES];
static u32 s_frame_count;

void gpu_frames_begin() { s_frame_count = 0; }

void gpu_frames_push(f32 frame_ms) {
    if (s_frame_count < GPU_MAX_FRAMES) s_frames[s_frame_count++] = frame_ms;
}

static f32 percentile(const f32* sorted, u32 n, f64 q) {
    if (n == 0) return 0.0f;
    u32 i = (u32)(q * (n - 1) + 0.5);
    return sorted[i < n ? i : n - 1];
}

void gpu_frames_report(BenchResult* out, f64 work_per_frame, const char* work_name, const char* work_unit, f64 unit_div) {
    const u32 n = s_frame_count;
    res_begin(out, RESULT_VALUES);
    if (n == 0) { res_fail(out, "no frames measured"); return; }
    f64 total_ms = 0.0;
    for (u32 i = 0; i < n; ++i) total_ms += s_frames[i];
    memcpy(s_sorted, s_frames, n * sizeof(f32));
    std::sort(s_sorted, s_sorted + n);
    const f64 avg_ms = total_ms / n;
    const f64 p99 = percentile(s_sorted, n, 0.99), p999 = percentile(s_sorted, n, 0.999);
    // Frame times only; the GUI shows the equivalent fps next to them (avg = average FPS, p99 = 1% low, ...).
    res_add(out, "Avg frame time", "ms", avg_ms);
    res_add(out, "p99 frame time", "ms", p99);
    res_add(out, "p99.9 frame time", "ms", p999);
    if (work_name && total_ms > 0.0) res_add(out, work_name, work_unit, work_per_frame * n / (total_ms / 1000.0) / unit_div);
}

// ---- compute reporting ------------------------------------------------------------------------------------

f64 gpu_rate(f64 work, const f64* dispatch_ms, u32 n) {
    f64 total = 0.0;
    for (u32 i = 0; i < n; ++i) total += dispatch_ms[i];
    return total > 0.0 ? work * n / (total / 1000.0) : 0.0;
}

void gpu_report_compute(BenchResult* out, const char* unit, f64 unit_div, f64 work_per_dispatch,
                        const f64* dispatch_ms, u32 n) {
    res_begin(out, RESULT_VALUES);
    if (n == 0) { res_fail(out, "no dispatches measured"); return; }
    f64 lo = dispatch_ms[0], hi = dispatch_ms[0], total = 0.0;
    for (u32 i = 0; i < n; ++i) {
        lo = dispatch_ms[i] < lo ? dispatch_ms[i] : lo;
        hi = dispatch_ms[i] > hi ? dispatch_ms[i] : hi;
        total += dispatch_ms[i];
    }
    res_add(out, "Throughput", unit, gpu_rate(work_per_dispatch, dispatch_ms, n) / unit_div);
    res_add(out, "Throughput (fastest dispatch)", unit, lo > 0.0 ? work_per_dispatch / (lo / 1000.0) / unit_div : 0.0);
    res_add(out, "Dispatch time min", "ms", lo);
    res_add(out, "Dispatch time avg", "ms", total / n);
    res_add(out, "Dispatch time max", "ms", hi);
}
