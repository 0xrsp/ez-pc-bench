// D3D12 GPU memory hierarchy (gpu_ext.h): pointer-chase latency and read bandwidth vs working-set size,
// both as series like the CPU memory suite.
#include "gpu/gpu_d3d12_ext.h"
#include "core/results.h"
#include <stdlib.h>
#include <string.h>

#include "cs_chase_dxil.h"
#include "cs_chase_init_dxil.h"
#include "cs_sweep_dxil.h"
#include "cs_mem_fill_dxil.h"

#define DXIL_BLOB(name) g_##name##_dxil, (u32)sizeof(g_##name##_dxil)

constexpr u32 MAX_TIMES = 1u << 14;
static f64 s_ms[MAX_TIMES];

struct MemRes {
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso[2];
    ID3D12Resource* buf[3];
};

static void mem_free(D3d* d, MemRes* r) {
    d3d_wait_idle(d);
    for (ID3D12Resource*& b : r->buf) d3d_release(b);
    for (ID3D12PipelineState*& p : r->pso) d3d_release(p);
    d3d_release(r->root);
}

static ID3D12Resource* uav_buffer(D3d* d, u64 bytes) {
    return d3d_buffer(d, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

// Single-thread chase; a short probe sizes each point's dispatches to ~GE_CHASE_TARGET_MS.
static void latency_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u64 cap = gpu_ext_mem_cap(p);
    const u32 max_lines = (u32)(cap / GE_CHASE_STRIDE);
    MemRes r = {};
    r.root = d3x_root(d);
    if (r.root) {
        r.pso[0] = d3x_compute_pso(d, r.root, DXIL_BLOB(cs_chase));
        r.pso[1] = d3x_compute_pso(d, r.root, DXIL_BLOB(cs_chase_init));
    }
    r.buf[0] = uav_buffer(d, cap);                                     // chain
    r.buf[1] = d3d_buffer(d, (u64)max_lines * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ);           // successor list
    r.buf[2] = uav_buffer(d, 256);                                     // sink
    u32* mapped = nullptr;
    u32* next = (u32*)malloc((u64)max_lines * 4);      // built in cached RAM: the shuffle reads randomly
    const D3D12_RANGE none = { 0, 0 };
    if (!next || !r.pso[0] || !r.pso[1] || !r.buf[0] || !r.buf[2] || !r.buf[1] || FAILED(r.buf[1]->Map(0, &none, (void**)&mapped))) {
        res_fail(out, "could not create the %llu MiB chain buffer / pipelines", cap / MiB);
        free(next);
        mem_free(d, &r);
        return;
    }
    const u32 chase_set = d3d_set_alloc(d), init_set = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, chase_set, 0, r.buf[2], 256);
    d3d_set_buffer_srv(d, chase_set, 0, r.buf[0], cap);
    d3d_set_buffer_uav(d, init_set, 0, r.buf[0], cap);
    d3d_set_buffer_srv(d, init_set, 0, r.buf[1], (u64)max_lines * 4);

    u64 sizes[GE_MEM_MAX_POINTS];
    f64 ns[GE_MEM_MAX_POINTS] = {};
    u32 count = 0;
    for (u64 s = GE_CHASE_MIN; s <= cap && count < GE_MEM_MAX_POINTS; s *= 2) sizes[count++] = s;
    const f64 seconds = gpu_ext_point_seconds(p);
    live_phase(live, "Pointer chase", "ns");
    bool gpu_ok = true;
    for (u32 i = 0; i < count && !cancel->load(std::memory_order_relaxed); ++i) {
        const u32 lines = (u32)(sizes[i] / GE_CHASE_STRIDE);
        gpu_ext_make_chain(next, lines);
        memcpy(mapped, next, (u64)lines * 4);
        const D3xDispatch init = { r.root, r.pso[1], init_set, { { lines, GE_CHASE_STRIDE, 0, 0 }, {} }, (lines + 255) / 256, 1 };
        d3x_once(d, &init);
        // Walk part of the chain once (fills caches / TLBs), then probe the latency with 4096 steps.
        const u32 warm_steps = (lines < (1u << 18) ? lines : (1u << 18)) / 4 * 4 + 4;
        D3xDispatch dp = { r.root, r.pso[0], chase_set, { { warm_steps, 0, 0, 0 }, {} }, 1, 1 };
        d3x_once(d, &dp);
        dp.pc.u[0] = 4096;
        if (d3x_timed(d, &dp, 0.0, 1e-9, 0.0, "Pointer chase", "ns", 1.0, live, cancel, 0, 0, s_ms, 1) != 1) {
            if (!cancel->load(std::memory_order_relaxed)) gpu_ok = false;
            break;
        }
        const f64 probe_ns = s_ms[0] * 1e6 / 4096;
        f64 steps = GE_CHASE_TARGET_MS * 1e6 / (probe_ns > 1.0 ? probe_ns : 1.0);
        steps = steps < 1024 ? 1024 : steps > (1 << 22) ? (1 << 22) : steps;
        dp.pc.u[0] = (u32)steps / 4 * 4;
        const i32 n = d3x_timed(d, &dp, 0.0, seconds, 0.0, "Pointer chase", "ns", 1.0, live, cancel, (f32)i / count,
                                (f32)(i + 1) / count, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        f64 total = 0.0;
        for (i32 k = 0; k < n; ++k) total += s_ms[k];
        ns[i] = n ? total / n * 1e6 / dp.pc.u[0] : 0.0;
        live_sample(live, ns[i]);
    }
    r.buf[1]->Unmap(0, nullptr);
    free(next);
    mem_free(d, &r);
    if (!gpu_ok) { res_fail(out, "GPU device removed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Working set", "bytes");
    for (u32 i = 0; i < count && ns[i] > 0.0; ++i) res_add(out, "Latency", "ns", ns[i], (f64)sizes[i]);
}

static void sweep_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u64 cap = gpu_ext_mem_cap(p);
    MemRes r = {};
    r.root = d3x_root(d);
    if (r.root) {
        r.pso[0] = d3x_compute_pso(d, r.root, DXIL_BLOB(cs_sweep));
        r.pso[1] = d3x_compute_pso(d, r.root, DXIL_BLOB(cs_mem_fill));
    }
    r.buf[0] = uav_buffer(d, cap);
    r.buf[1] = uav_buffer(d, 256 * 4);
    if (!r.pso[0] || !r.pso[1] || !r.buf[0] || !r.buf[1]) {
        res_fail(out, "could not create the %llu MiB buffer / pipelines", cap / MiB);
        mem_free(d, &r);
        return;
    }
    const u32 sweep_set = d3d_set_alloc(d), fill_set = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, sweep_set, 0, r.buf[1], 256 * 4);
    d3d_set_buffer_srv(d, sweep_set, 0, r.buf[0], cap);
    d3d_set_buffer_uav(d, fill_set, 0, r.buf[0], cap);
    const D3xDispatch fill = { r.root, r.pso[1], fill_set, { { (u32)(cap / 16), GE_SWEEP_GROUPS * 256, 0, 0 }, {} }, GE_SWEEP_GROUPS, 1 };
    d3x_once(d, &fill);

    u64 sizes[GE_MEM_MAX_POINTS];
    f64 rate[GE_MEM_MAX_POINTS] = {};
    u32 count = 0;
    for (u64 s = GE_SWEEP_MIN; s <= cap && count < GE_MEM_MAX_POINTS; s *= 2) sizes[count++] = s;
    const f64 seconds = gpu_ext_point_seconds(p);
    bool gpu_ok = true;
    for (u32 i = 0; i < count && !cancel->load(std::memory_order_relaxed); ++i) {
        const D3xDispatch dp = { r.root, r.pso[0], sweep_set, { { (u32)(sizes[i] / 16 - 1), GE_SWEEP_LOADS, 0, 0 }, {} },
                                 GE_SWEEP_GROUPS, 1 };
        const i32 n = d3x_timed(d, &dp, i == 0 ? gpu_warmup_seconds(p) : 0.02, seconds, GE_SWEEP_BYTES_PER_DISPATCH, "Read sweep",
                                "GB/s", 1e9, live, cancel, (f32)i / count, (f32)(i + 1) / count, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        rate[i] = gpu_rate(GE_SWEEP_BYTES_PER_DISPATCH, s_ms, (u32)n) / 1e9;
    }
    mem_free(d, &r);
    if (!gpu_ok) { res_fail(out, "GPU device removed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Working set", "bytes");
    for (u32 i = 0; i < count && rate[i] > 0.0; ++i) res_add(out, "Read", "GB/s", rate[i], (f64)sizes[i]);
}

void d3x_memory_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (test == GT_MEM_LATENCY) latency_test(d, p, live, out, cancel);
    else sweep_test(d, p, live, out, cancel);
}
