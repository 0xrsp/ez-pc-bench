// Vulkan GPU memory hierarchy (gpu_ext.h): pointer-chase latency and read bandwidth vs working-set size,
// both as series like the CPU memory suite.
#include "gpu/gpu_vk_ext.h"
#include "core/results.h"
#include <stdlib.h>
#include <string.h>

#include "cs_chase_spv.h"
#include "cs_chase_init_spv.h"
#include "cs_sweep_spv.h"
#include "cs_mem_fill_spv.h"

#define SPV_BLOB(name) g_##name##_spv, (u32)sizeof(g_##name##_spv), #name

constexpr u32 MAX_TIMES = 1u << 14;
static f64 s_ms[MAX_TIMES];

// u0 = destination / sink, t0 = source (binding 8).
static const VkxBinding BIND_U0_T0[] = {
    { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr },
    { 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr },
};

struct MemRes {
    VkxPipe pipe[2];
    VkBuf buf[3];
    VkDescriptorPool pool;
};

static void res_free(VkCtx* v, MemRes* r) {
    vkDeviceWaitIdle(v->dev);
    if (r->pool) vkDestroyDescriptorPool(v->dev, r->pool, nullptr);
    for (VkxPipe& p : r->pipe) vkx_pipe_destroy(v, &p);
    for (VkBuf& b : r->buf) buf_destroy(v, &b);
    *r = {};
}

static VkDescriptorSet make_set(VkCtx* v, MemRes* r, u32 pipe, const VkBuf* u0, const VkBuf* t0) {
    const VkDescriptorSet set = vkx_set(v, r->pool, &r->pipe[pipe]);
    if (!set) return VK_NULL_HANDLE;
    desc_buffer(v, set, 0, u0);
    desc_buffer(v, set, 8, t0);
    return set;
}

// The single-thread chase: a short probe sizes each point's dispatches to ~GE_CHASE_TARGET_MS, then timed
// dispatches for gpu_ext_point_seconds(). ns per load = dispatch time / steps.
static void latency_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u64 cap = gpu_ext_mem_cap(p);
    const u32 max_lines = (u32)(cap / GE_CHASE_STRIDE);
    MemRes r = {};
    const VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bool ok = vkx_compute_pipe(v, &r.pipe[0], SPV_BLOB(cs_chase), BIND_U0_T0, 2) &&
              vkx_compute_pipe(v, &r.pipe[1], SPV_BLOB(cs_chase_init), BIND_U0_T0, 2) &&
              buf_create(v, &r.buf[0], cap, st, MEM_DEVICE) &&                          // chain
              buf_create(v, &r.buf[1], (u64)max_lines * 4, st, MEM_UPLOAD) &&           // successor list
              buf_create(v, &r.buf[2], 256, st, MEM_DEVICE);                            // sink
    r.pool = ok ? vkx_pool(v) : VK_NULL_HANDLE;
    const VkDescriptorSet chase_set = ok ? make_set(v, &r, 0, &r.buf[2], &r.buf[0]) : VK_NULL_HANDLE;
    const VkDescriptorSet init_set = ok ? make_set(v, &r, 1, &r.buf[0], &r.buf[1]) : VK_NULL_HANDLE;
    u32* next = (u32*)malloc((u64)max_lines * 4);      // built in cached RAM: the shuffle reads randomly
    if (!chase_set || !init_set || !next) {
        res_fail(out, "could not create the %llu MiB chain buffer / pipelines", cap / MiB);
        free(next);
        res_free(v, &r);
        return;
    }

    u64 sizes[GE_MEM_MAX_POINTS];
    f64 ns[GE_MEM_MAX_POINTS] = {};
    u32 count = 0;
    for (u64 s = GE_CHASE_MIN; s <= cap && count < GE_MEM_MAX_POINTS; s *= 2) sizes[count++] = s;
    const f64 seconds = gpu_ext_point_seconds(p);
    live_phase(live, "Pointer chase", "ns");
    bool gpu_ok = true;
    for (u32 i = 0; i < count && gpu_ok && !cancel->load(std::memory_order_relaxed); ++i) {
        const u32 lines = (u32)(sizes[i] / GE_CHASE_STRIDE);
        gpu_ext_make_chain(next, lines);
        memcpy(r.buf[1].map, next, (u64)lines * 4);     // MEM_UPLOAD is host-coherent
        const VkxDispatch init = { &r.pipe[1], init_set, { { lines, GE_CHASE_STRIDE, 0, 0 }, {} }, (lines + 255) / 256, 1 };
        // Walk part of the chain once (fills caches / TLBs), then probe the latency with 4096 steps.
        const u32 warm_steps = (lines < (1u << 18) ? lines : (1u << 18)) / 4 * 4 + 4;
        VkxDispatch d = { &r.pipe[0], chase_set, { { warm_steps, 0, 0, 0 }, {} }, 1, 1 };
        gpu_ok = vkx_once(v, &init) && vkx_once(v, &d);
        d.pc.u[0] = 4096;
        if (gpu_ok) gpu_ok = vkx_timed(v, &d, 0.0, 1e-9, 0.0, "Pointer chase", "ns", 1.0, live, cancel, 0, 0, s_ms, 1) == 1;
        if (!gpu_ok) break;
        const f64 probe_ns = s_ms[0] * 1e6 / 4096;
        f64 steps = GE_CHASE_TARGET_MS * 1e6 / (probe_ns > 1.0 ? probe_ns : 1.0);
        steps = steps < 1024 ? 1024 : steps > (1 << 22) ? (1 << 22) : steps;
        d.pc.u[0] = (u32)steps / 4 * 4;
        const i32 n = vkx_timed(v, &d, 0.0, seconds, 0.0, "Pointer chase", "ns", 1.0, live, cancel,
                                (f32)i / count, (f32)(i + 1) / count, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        f64 total = 0.0;
        for (i32 k = 0; k < n; ++k) total += s_ms[k];
        ns[i] = n ? total / n * 1e6 / d.pc.u[0] : 0.0;
        live_sample(live, ns[i]);
    }
    free(next);
    res_free(v, &r);
    if (!gpu_ok) { res_fail(out, "GPU error (device lost or timeout)"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Working set", "bytes");
    for (u32 i = 0; i < count && ns[i] > 0.0; ++i) res_add(out, "Latency", "ns", ns[i], (f64)sizes[i]);
}

static void sweep_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u64 cap = gpu_ext_mem_cap(p);
    MemRes r = {};
    const VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bool ok = vkx_compute_pipe(v, &r.pipe[0], SPV_BLOB(cs_sweep), BIND_U0_T0, 2) &&
              vkx_compute_pipe(v, &r.pipe[1], SPV_BLOB(cs_mem_fill), BIND_U0_T0, 2) &&
              buf_create(v, &r.buf[0], cap, st, MEM_DEVICE) &&      // working set
              buf_create(v, &r.buf[1], 256 * 4, st, MEM_DEVICE);    // sink
    r.pool = ok ? vkx_pool(v) : VK_NULL_HANDLE;
    const VkDescriptorSet sweep_set = ok ? make_set(v, &r, 0, &r.buf[1], &r.buf[0]) : VK_NULL_HANDLE;
    const VkDescriptorSet fill_set = ok ? make_set(v, &r, 1, &r.buf[0], &r.buf[1]) : VK_NULL_HANDLE;
    const VkxDispatch fill = { &r.pipe[1], fill_set, { { (u32)(cap / 16), GE_SWEEP_GROUPS * 256, 0, 0 }, {} }, GE_SWEEP_GROUPS, 1 };
    if (!sweep_set || !fill_set || !vkx_once(v, &fill)) {
        res_fail(out, "could not create / fill the %llu MiB buffer", cap / MiB);
        res_free(v, &r);
        return;
    }

    u64 sizes[GE_MEM_MAX_POINTS];
    f64 rate[GE_MEM_MAX_POINTS] = {};
    u32 count = 0;
    for (u64 s = GE_SWEEP_MIN; s <= cap && count < GE_MEM_MAX_POINTS; s *= 2) sizes[count++] = s;
    const f64 seconds = gpu_ext_point_seconds(p);
    bool gpu_ok = true;
    for (u32 i = 0; i < count && !cancel->load(std::memory_order_relaxed); ++i) {
        const VkxDispatch d = { &r.pipe[0], sweep_set, { { (u32)(sizes[i] / 16 - 1), GE_SWEEP_LOADS, 0, 0 }, {} }, GE_SWEEP_GROUPS, 1 };
        const i32 n = vkx_timed(v, &d, i == 0 ? gpu_warmup_seconds(p) : 0.02, seconds, GE_SWEEP_BYTES_PER_DISPATCH, "Read sweep",
                                "GB/s", 1e9, live, cancel, (f32)i / count, (f32)(i + 1) / count, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        rate[i] = gpu_rate(GE_SWEEP_BYTES_PER_DISPATCH, s_ms, (u32)n) / 1e9;
    }
    res_free(v, &r);
    if (!gpu_ok) { res_fail(out, "GPU error (device lost or timeout)"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Working set", "bytes");
    for (u32 i = 0; i < count && rate[i] > 0.0; ++i) res_add(out, "Read", "GB/s", rate[i], (f64)sizes[i]);
}

void vk_ext_memory_test(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                        const CancelFlag* cancel) {
    if (test == GT_MEM_LATENCY) latency_test(v, p, live, out, cancel);
    else sweep_test(v, p, live, out, cancel);
}
