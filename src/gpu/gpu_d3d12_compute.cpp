// D3D12 compute tests (ALU, VRAM, LDS) and host <-> GPU transfers. Contract: gpu_common.h.
#include "gpu/gpu_d3d12_internal.h"
#include "core/results.h"
#include "core/timer.h"
#include <string.h>

constexpr u32 MAX_TIMINGS = 1u << 18;
static f64 s_ms[MAX_TIMINGS];                   // per-dispatch / per-copy times of the current phase
constexpr f64 BATCH_TARGET_MS = 20.0;           // GPU time per submitted batch
constexpr f64 LIVE_INTERVAL = 0.05;

struct Dispatch {
    D3dPipe pipe;
    u32 set;
    GpuParams pc;
    u32 groups_x, groups_y;
    f64 work;                                   // work units per dispatch (flops, bytes, ...)
};

struct Phase {
    const char* name;                           // live phase while measuring
    const char* unit;
    f64 unit_div;
    f32 progress_from, progress_to;
};

static void live_rate(BenchLive* live, f64* last_t, f64* work, f64* ms, f64 now, f64 unit_div) {
    if (now - *last_t < LIVE_INTERVAL || *ms <= 0.0) return;
    live_sample(live, *work / (*ms / 1000.0) / unit_div);
    *last_t = now;
    *work = *ms = 0.0;
}

// Warm-up, then timed batches of dispatches until duration_ms. Every dispatch is bracketed by GPU
// timestamps; a UAV barrier between dispatches serializes them. Returns the measured count (times in s_ms).
static u32 run_dispatches(D3d* d, const Dispatch* dp, const Phase* ph, const BenchParams* p, BenchLive* live,
                          const CancelFlag* cancel) {
    const f64 warmup = gpu_warmup_seconds(p), duration = p->duration_ms / 1000.0;
    u32 batch = 1, n = 0, counter = 0;
    bool measuring = false;
    live_phase(live, "Warm-up", ph->unit);
    const f64 t_start = timer_now();
    f64 t_measure = t_start, last_sample = t_start, live_work = 0.0, live_ms = 0.0;
    while (!cancel->load(std::memory_order_relaxed)) {
        d3d_begin(d, 0);
        ID3D12GraphicsCommandList* list = d->list;
        list->SetPipelineState(d->pipe[dp->pipe]);
        GpuParams pc = dp->pc;
        list->EndQuery(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (u32 i = 0; i < batch; ++i) {
            pc.u[2] = dp->pipe == DP_BW_WRITE ? counter++ : pc.u[2];
            d3d_bind_compute(d, list, dp->set, &pc);
            list->Dispatch(dp->groups_x, dp->groups_y, 1);
            d3d_uav_barrier(list);
            list->EndQuery(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i + 1);
        }
        list->ResolveQueryData(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, batch + 1, d->ts_readback, 0);
        d3d_wait(&d->direct, d3d_submit(d));

        f64 batch_ms = 0.0;
        for (u32 i = 0; i < batch; ++i) {
            const f64 ms = (f64)(d->ts_data[i + 1] - d->ts_data[i]) * d->ts_ms_per_tick;
            batch_ms += ms;
            if (measuring && n < MAX_TIMINGS) s_ms[n++] = ms;
        }
        const f64 now = timer_now();
        live_work += dp->work * batch;
        live_ms += batch_ms;
        live_rate(live, &last_sample, &live_work, &live_ms, now, ph->unit_div);

        const f64 frac = (now - t_start) / (warmup + duration);
        live_progress(live, ph->progress_from + (ph->progress_to - ph->progress_from) * (f32)(frac < 1.0 ? frac : 1.0));
        if (!measuring && now - t_start >= warmup) {
            measuring = true;
            t_measure = now;
            live_phase(live, ph->name, ph->unit);
        } else if (measuring && now - t_measure >= duration) {
            break;
        }
        // Size the next batch for ~BATCH_TARGET_MS of GPU time.
        const f64 per = batch_ms > 0.0 ? batch_ms / batch : BATCH_TARGET_MS;
        u32 next = (u32)(BATCH_TARGET_MS / (per > 1e-3 ? per : 1e-3));
        batch = next < 1 ? 1 : next > D3D_MAX_TS - 1 ? D3D_MAX_TS - 1 : next;
    }
    return n;
}

static void alu_test(D3d* d, D3dPipe pipe, const char* unit, const BenchParams* p, BenchLive* live,
                     BenchResult* out, const CancelFlag* cancel) {
    Dispatch dp = {};
    dp.pipe = pipe;
    dp.set = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, dp.set, 0, d->sink, (u64)GC_ALU_GROUPS * 256 * 4);
    dp.pc = { { GC_ALU_ITERS, 3, 5, 7 }, { 1.0f, 1.25f, 1.5f, 1.75f } };
    dp.groups_x = GC_ALU_GROUPS;
    dp.groups_y = 1;
    dp.work = GC_ALU_OPS_PER_DISPATCH;
    const Phase ph = { "Measuring", unit, 1e12, 0.0f, 1.0f };
    const u32 n = run_dispatches(d, &dp, &ph, p, live, cancel);
    gpu_report_compute(out, unit, 1e12, dp.work, s_ms, n);
}

static void lds_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    Dispatch dp = {};
    dp.pipe = DP_LDS;
    dp.set = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, dp.set, 0, d->sink, (u64)GC_ALU_GROUPS * 256 * 4);
    dp.pc = { { GC_LDS_ITERS, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
    dp.groups_x = GC_LDS_GROUPS;
    dp.groups_y = 1;
    dp.work = GC_LDS_BYTES_PER_DISPATCH;
    const Phase ph = { "Measuring", "GB/s", 1e9, 0.0f, 1.0f };
    const u32 n = run_dispatches(d, &dp, &ph, p, live, cancel);
    gpu_report_compute(out, "GB/s", 1e9, dp.work, s_ms, n);
}

static void vram_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Allocating", "");
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* a = d3d_buffer(d, GC_VRAM_BYTES, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* b = d3d_buffer(d, GC_VRAM_BYTES, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON);
    if (!a || !b) {
        d3d_release(a);
        d3d_release(b);
        res_fail(out, "out of GPU memory (2 x %u MiB)", (u32)(GC_VRAM_BYTES / MiB));
        return;
    }
    const u32 threads = GC_BW_GROUPS * 256;
    const u32 elems = (u32)(GC_VRAM_BYTES / 16);
    const u32 set_fill = d3d_set_alloc(d), set_rw = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, set_fill, 0, a, GC_VRAM_BYTES);
    d3d_set_buffer_srv(d, set_rw, 0, a, GC_VRAM_BYTES);
    d3d_set_buffer_uav(d, set_rw, 0, b, GC_VRAM_BYTES);

    // Fill the source with non-trivial data (untimed).
    const GpuParams fill_pc = { { elems, threads, 0x1234u, 0 }, {} };
    d3d_begin(d, 0);
    d->list->SetPipelineState(d->pipe[DP_BW_WRITE]);
    d3d_bind_compute(d, d->list, set_fill, &fill_pc);
    d->list->Dispatch(GC_BW_GROUPS, 1, 1);
    d3d_wait(&d->direct, d3d_submit(d));

    struct { D3dPipe pipe; const char* name; f64 bytes; } phases[3] = {
        { DP_BW_READ, "Read", (f64)GC_VRAM_BYTES },
        { DP_BW_WRITE, "Write", (f64)GC_VRAM_BYTES },
        { DP_BW_COPY, "Copy", 2.0 * GC_VRAM_BYTES },
    };
    f64 rates[3] = {};
    for (u32 i = 0; i < 3 && !cancel->load(std::memory_order_relaxed); ++i) {
        Dispatch dp = {};
        dp.pipe = phases[i].pipe;
        dp.set = set_rw;
        dp.pc = { { elems, threads, 0, 0 }, {} };
        dp.groups_x = GC_BW_GROUPS;
        dp.groups_y = 1;
        dp.work = phases[i].bytes;
        const Phase ph = { phases[i].name, "GB/s", 1e9, i / 3.0f, (i + 1) / 3.0f };
        const u32 n = run_dispatches(d, &dp, &ph, p, live, cancel);
        rates[i] = gpu_rate(dp.work, s_ms, n) / 1e9;
    }
    d3d_wait_idle(d);
    d3d_release(a);
    d3d_release(b);
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < 3; ++i) res_add(out, phases[i].name, "GB/s", rates[i]);
}

void d3d_compute_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                      const CancelFlag* cancel) {
    switch (test) {
    case GT_FP32: alu_test(d, DP_FP32, "TFLOPS", p, live, out, cancel); break;
    case GT_FP16:
        if (!d->fp16) { res_skip(out, "GPU/driver lacks SM 6.2 native 16-bit shader ops"); return; }
        alu_test(d, DP_FP16, "TFLOPS", p, live, out, cancel);
        break;
    case GT_INT32: alu_test(d, DP_INT32, "TIOPS", p, live, out, cancel); break;
    case GT_VRAM_BW: vram_test(d, p, live, out, cancel); break;
    case GT_LDS_BW: lds_test(d, p, live, out, cancel); break;
    default: break;
    }
}

// ---- transfer ---------------------------------------------------------------------------------------

// Times one pre-recorded copy list on the copy queue (wall clock, submit -> fence), warm-up then duration.
static u32 run_copies(D3d* d, ID3D12CommandList* copy, const Phase* ph, const BenchParams* p, BenchLive* live,
                      const CancelFlag* cancel) {
    const f64 warmup = gpu_warmup_seconds(p), duration = p->duration_ms / 1000.0;
    u32 n = 0;
    bool measuring = false;
    live_phase(live, "Warm-up", ph->unit);
    const f64 t_start = timer_now();
    f64 t_measure = t_start, last_sample = t_start, live_work = 0.0, live_ms = 0.0;
    while (!cancel->load(std::memory_order_relaxed)) {
        const f64 t0 = timer_now();
        d->copy.queue->ExecuteCommandLists(1, &copy);
        d3d_wait(&d->copy, d3d_signal(&d->copy));
        const f64 now = timer_now();
        const f64 ms = (now - t0) * 1000.0;
        if (measuring && n < MAX_TIMINGS) s_ms[n++] = ms;
        live_work += (f64)GC_TRANSFER_BYTES;
        live_ms += ms;
        live_rate(live, &last_sample, &live_work, &live_ms, now, ph->unit_div);
        const f64 frac = (now - t_start) / (warmup + duration);
        live_progress(live, ph->progress_from + (ph->progress_to - ph->progress_from) * (f32)(frac < 1.0 ? frac : 1.0));
        if (!measuring && now - t_start >= warmup) {
            measuring = true;
            t_measure = now;
            live_phase(live, ph->name, ph->unit);
        } else if (measuring && now - t_measure >= duration) {
            break;
        }
    }
    return n;
}

// CPU-cached system-memory buffer (custom heap: L0 + write-back), used for both directions. Write-combined
// host heaps (the UPLOAD heap and even custom L0 + write-combine) were placed in VRAM behind the resizable
// BAR by the NVIDIA driver: "upload" then measured a VRAM -> VRAM copy (38 GB/s copy queue, 160 GB/s
// direct queue over a PCIe 4.0 x8 link). Write-back memory always stays in system RAM.
static ID3D12Resource* host_buffer(D3d* d, u64 bytes) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    if (FAILED(d->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

void d3d_transfer_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Allocating", "");
    ID3D12Resource* up = host_buffer(d, GC_TRANSFER_BYTES);
    ID3D12Resource* dev = d3d_buffer(d, GC_TRANSFER_BYTES, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* rb = host_buffer(d, GC_TRANSFER_BYTES);
    // Two lists recorded once and re-executed (one allocator each: only one list may record per allocator).
    ID3D12CommandAllocator* allocs[2] = {};
    ID3D12GraphicsCommandList* lists[2] = {};
    bool ok = up && dev && rb;
    for (u32 i = 0; i < 2 && ok; ++i)
        ok = SUCCEEDED(d->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocs[i]))) &&
             SUCCEEDED(d->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocs[i], nullptr, IID_PPV_ARGS(&lists[i])));

    void* ptr = nullptr;
    if (ok && SUCCEEDED(up->Map(0, nullptr, &ptr))) {
        u64* w = (u64*)ptr;
        for (u64 i = 0; i < GC_TRANSFER_BYTES / 8; ++i) w[i] = i * 0x9E3779B97F4A7C15ull;
        up->Unmap(0, nullptr);
    } else {
        ok = false;
    }

    f64 rates[2] = {};
    bool data_ok = true;
    if (ok) {
        lists[0]->CopyBufferRegion(dev, 0, up, 0, GC_TRANSFER_BYTES);
        lists[0]->Close();
        lists[1]->CopyBufferRegion(rb, 0, dev, 0, GC_TRANSFER_BYTES);
        lists[1]->Close();
        const Phase phases[2] = { { "Upload", "GB/s", 1e9, 0.0f, 0.5f }, { "Readback", "GB/s", 1e9, 0.5f, 1.0f } };
        for (u32 i = 0; i < 2 && !cancel->load(std::memory_order_relaxed); ++i) {
            const u32 n = run_copies(d, lists[i], &phases[i], p, live, cancel);
            rates[i] = gpu_rate((f64)GC_TRANSFER_BYTES, s_ms, n) / 1e9;
        }
        // Spot-check the round trip: readback must equal what was uploaded.
        void* rp = nullptr;
        const D3D12_RANGE range = { 0, 4096 };
        if (rates[1] > 0.0 && SUCCEEDED(rb->Map(0, &range, &rp))) {
            const u64* r = (const u64*)rp;
            for (u64 i = 0; i < 512; ++i) data_ok &= r[i] == i * 0x9E3779B97F4A7C15ull;
            const D3D12_RANGE none = { 0, 0 };
            rb->Unmap(0, &none);
        }
    }
    d3d_wait_idle(d);
    for (u32 i = 0; i < 2; ++i) {
        d3d_release(lists[i]);
        d3d_release(allocs[i]);
    }
    d3d_release(up);
    d3d_release(dev);
    d3d_release(rb);
    if (!ok) { res_fail(out, "transfer setup failed (out of memory?)"); return; }
    res_begin(out, RESULT_VALUES);
    res_add(out, "Upload", "GB/s", rates[0]);
    res_add(out, "Readback", "GB/s", rates[1]);
    if (!data_ok) res_fail(out, "readback data mismatch");
}
