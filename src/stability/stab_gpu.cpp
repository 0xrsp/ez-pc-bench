// GPU stability tests on a small, self-contained Direct3D 12 path (independent of the benchmark backends in
// src/gpu/): VRAM pattern test (STAB_VRAM), known-answer compute + render (STAB_GPU) and the maximum-power load
// (STAB_POWER). Shaders: stab.hlsl. Every batch ends with a fence wait and a readback of the error counters;
// a removed device (driver reset after a hang / fault) counts as an error and ends the test.
#include "stability/stab_internal.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/timer.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <immintrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cs_stab_fill_dxil.h"
#include "cs_stab_check_dxil.h"
#include "cs_stab_compute_dxil.h"
#include "vs_stab_full_dxil.h"
#include "ps_stab_render_dxil.h"
#include "cs_stab_render_check_dxil.h"
#include "cs_stab_power_dxil.h"

#pragma clang fp contract(off)     // the CPU references must round every step like `precise` HLSL

#define DXIL(name) D3D12_SHADER_BYTECODE{ g_##name##_dxil, sizeof(g_##name##_dxil) }

// Mirrors stab.hlsl.
constexpr u32 ERR_TOTAL = 0, ERR_CHUNK = 16, ERR_LOG = 128, ERR_LOG_MAX = 16, ERR_WORDS = 256;
constexpr u32 FILL_THREADS = 1024 * 256;            // grid-stride dispatches
constexpr u32 COMPUTE_THREADS = 256 * 256;
constexpr u32 COMPUTE_ITERS = 512;
constexpr u32 COMPUTE_SEEDS = 4;
constexpr u32 RENDER_W = 1024;
constexpr u32 RENDER_ROUNDS = 48;
constexpr u32 RENDER_INSTANCES = 4;
constexpr u32 POWER_ITERS = 256;
constexpr f64 BATCH_TARGET_S = 0.05;                // aim for ~50 ms of GPU work per submission

enum Pso : u8 { P_FILL, P_CHECK, P_COMPUTE, P_RENDER, P_RENDER_CHECK, P_POWER, P_COUNT };

struct Gpu {
    IDXGIFactory4* factory;
    IDXGIAdapter3* adapter;
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    u64 fence_value;
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso[P_COUNT];
    ID3D12Resource* errs;           // ERR_WORDS uints, UAV
    ID3D12Resource* errs_rb;        // readback copy
    u32 seen[ERR_WORDS];            // counters as of the last readback
    bool lost;                      // device removed / hung: stop
    StabTest test;
};

template <class T> static void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static ID3D12Resource* make_buffer(Gpu* g, u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(g->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

static void uav_barrier(ID3D12GraphicsCommandList* l) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    l->ResourceBarrier(1, &b);
}

static void gpu_close(Gpu* g) {
    if (g->dev && g->fence && !g->lost) {      // drain before releasing anything
        g->queue->Signal(g->fence, ++g->fence_value);
        if (g->fence->GetCompletedValue() < g->fence_value) {
            g->fence->SetEventOnCompletion(g->fence_value, g->event);
            WaitForSingleObject(g->event, 5000);
        }
    }
    for (u32 i = 0; i < P_COUNT; ++i) release(g->pso[i]);
    release(g->errs_rb);
    release(g->errs);
    release(g->root);
    release(g->list);
    release(g->alloc);
    release(g->fence);
    release(g->queue);
    release(g->dev);
    release(g->adapter);
    release(g->factory);
    if (g->event) CloseHandle(g->event);
    g->event = nullptr;
}

static ID3D12PipelineState* compute_pso(Gpu* g, D3D12_SHADER_BYTECODE cs) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g->root;
    d.CS = cs;
    ID3D12PipelineState* p = nullptr;
    g->dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&p));
    return p;
}

static bool gpu_open(Gpu* g, StabTest test, char* err, u32 err_size) {
    memset(g, 0, sizeof(*g));
    g->test = test;
    const u32 idx = g_stab.cfg.gpu_adapter;
    if (idx >= g_sys.gpu_count) { snprintf(err, err_size, "no GPU adapter"); return false; }
    const GpuInfo* gi = &g_sys.gpus[idx];
    LUID luid;
    luid.LowPart = gi->luid_low;
    luid.HighPart = gi->luid_high;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g->factory))) ||
        FAILED(g->factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g->adapter)))) {
        snprintf(err, err_size, "GPU %s not found", gi->name);
        return false;
    }
    HRESULT hr = D3D12CreateDevice(g->adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g->dev));
    if (FAILED(hr)) { snprintf(err, err_size, "Direct3D 12 not available on %s (0x%08X)", gi->name, (u32)hr); return false; }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g->dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g->queue))) ||
        FAILED(g->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g->alloc))) ||
        FAILED(g->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g->alloc, nullptr, IID_PPV_ARGS(&g->list))) ||
        FAILED(g->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->fence)))) {
        snprintf(err, err_size, "creating the command queue failed");
        return false;
    }
    g->list->Close();
    g->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Root signature: 8 constants (b0), root UAVs u0..u2.
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, 8 };
    for (u32 i = 1; i < 4; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor = { i - 1, 0 };
    }
    D3D12_ROOT_SIGNATURE_DESC rs = { 4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    ID3DBlob* rs_err = nullptr;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rs_err);
    release(rs_err);
    if (FAILED(hr) || FAILED(g->dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g->root)))) {
        release(blob);
        snprintf(err, err_size, "creating the root signature failed");
        return false;
    }
    release(blob);

    g->pso[P_FILL] = compute_pso(g, DXIL(cs_stab_fill));
    g->pso[P_CHECK] = compute_pso(g, DXIL(cs_stab_check));
    g->pso[P_COMPUTE] = compute_pso(g, DXIL(cs_stab_compute));
    g->pso[P_RENDER_CHECK] = compute_pso(g, DXIL(cs_stab_render_check));
    g->pso[P_POWER] = compute_pso(g, DXIL(cs_stab_power));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = {};
    gd.pRootSignature = g->root;
    gd.VS = DXIL(vs_stab_full);
    gd.PS = DXIL(ps_stab_render);
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = 0xFFFFFFFFu;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    gd.SampleDesc.Count = 1;
    g->dev->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&g->pso[P_RENDER]));
    for (u32 i = 0; i < P_COUNT; ++i)
        if (!g->pso[i]) { snprintf(err, err_size, "creating pipeline %u failed", i); return false; }

    g->errs = make_buffer(g, ERR_WORDS * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g->errs_rb = make_buffer(g, ERR_WORDS * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    if (!g->errs || !g->errs_rb) { snprintf(err, err_size, "out of GPU memory"); return false; }
    return true;
}

static void begin(Gpu* g) {
    g->alloc->Reset();
    g->list->Reset(g->alloc, nullptr);
    g->list->SetComputeRootSignature(g->root);
    g->list->SetGraphicsRootSignature(g->root);
}

static void lost(Gpu* g, HRESULT hr) {
    if (g->lost) return;
    g->lost = true;
    stab_error(g->test, -1, -1, 0, "GPU device removed / hung", 0, (u32)hr);
}

// Closes, submits and waits. Also copies the error counters for read_errors. False if the device is gone.
static bool submit(Gpu* g, bool read_errors) {
    if (read_errors) {
        uav_barrier(g->list);
        barrier(g->list, g->errs, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g->list->CopyResource(g->errs_rb, g->errs);
        barrier(g->list, g->errs, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (FAILED(g->list->Close())) { lost(g, E_FAIL); return false; }
    ID3D12CommandList* lists[] = { g->list };
    g->queue->ExecuteCommandLists(1, lists);
    g->queue->Signal(g->fence, ++g->fence_value);
    g->fence->SetEventOnCompletion(g->fence_value, g->event);
    // A healthy batch takes ~50 ms; give a slow or throttled GPU 10 s before calling it hung.
    for (u32 waited = 0; g->fence->GetCompletedValue() < g->fence_value; waited += 250) {
        const HRESULT hr = g->dev->GetDeviceRemovedReason();
        if (FAILED(hr)) { lost(g, hr); return false; }
        if (waited >= 10000) { lost(g, (HRESULT)0x887A0006 /* DXGI_ERROR_DEVICE_HUNG */); return false; }
        WaitForSingleObject(g->event, 250);
    }
    const HRESULT hr = g->dev->GetDeviceRemovedReason();
    if (FAILED(hr)) { lost(g, hr); return false; }
    return true;
}

// New errors since the last call: logged entries go through stab_error, the rest are added per chunk.
static void read_errors(Gpu* g, const char* const* chunk_what, const i32* chunk_map) {
    u32* e = nullptr;
    const D3D12_RANGE r = { 0, ERR_WORDS * 4 };
    if (FAILED(g->errs_rb->Map(0, &r, (void**)&e))) return;
    u32 cur[ERR_WORDS];
    memcpy(cur, e, sizeof(cur));
    const D3D12_RANGE none = { 0, 0 };
    g->errs_rb->Unmap(0, &none);
    if (cur[ERR_TOTAL] == g->seen[ERR_TOTAL]) return;
    u32 delta[64];
    for (u32 c = 0; c < 64; ++c) delta[c] = cur[ERR_CHUNK + c] - g->seen[ERR_CHUNK + c];
    const u32 logged_before = g->seen[ERR_TOTAL] < ERR_LOG_MAX ? g->seen[ERR_TOTAL] : ERR_LOG_MAX;
    const u32 logged_now = cur[ERR_TOTAL] < ERR_LOG_MAX ? cur[ERR_TOTAL] : ERR_LOG_MAX;
    for (u32 k = logged_before; k < logged_now; ++k) {
        const u32* l = &cur[ERR_LOG + k * 4];
        const u32 c = l[0] & 63;
        if (delta[c]) delta[c]--;
        stab_error(g->test, -1, chunk_map ? chunk_map[c] : (i32)c, (u64)l[1] * 4, chunk_what[c < 2 ? c : 0], l[2], l[3]);
    }
    for (u32 c = 0; c < 64; ++c)
        if (delta[c]) stab_add_errors(g->test, chunk_map ? chunk_map[c] : (i32)c, delta[c]);
    memcpy(g->seen, cur, sizeof(cur));
}

static void set_constants(Gpu* g, bool graphics, const u32 c[8]) {
    if (graphics) g->list->SetGraphicsRoot32BitConstants(0, 8, c, 0);
    else g->list->SetComputeRoot32BitConstants(0, 8, c, 0);
}

static void set_uavs(Gpu* g, ID3D12Resource* u0, ID3D12Resource* u2) {
    g->list->SetComputeRootUnorderedAccessView(1, (u0 ? u0 : g->errs)->GetGPUVirtualAddress());
    g->list->SetComputeRootUnorderedAccessView(2, g->errs->GetGPUVirtualAddress());
    g->list->SetComputeRootUnorderedAccessView(3, (u2 ? u2 : g->errs)->GetGPUVirtualAddress());
}

static u64 free_vram(Gpu* g) {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
    if (FAILED(g->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return 0;
    return mi.Budget > mi.CurrentUsage ? mi.Budget - mi.CurrentUsage : 0;
}

// ---- VRAM -------------------------------------------------------------------------------------------------

constexpr u32 VRAM_MAX_CHUNKS = 64;

static void vram_run(Gpu* g, f64 deadline) {
    StabTestState* ts = &g_stab.tests[STAB_VRAM];
    const u64 avail = free_vram(g);
    u64 want = g_stab.cfg.vram_bytes ? g_stab.cfg.vram_bytes : avail / 100 * (g_stab.cfg.vram_pct ? g_stab.cfg.vram_pct : 85);
    if (avail && want > avail) want = avail;
    u64 chunk = 256 * MiB;
    while (want / chunk > VRAM_MAX_CHUNKS) chunk *= 2;
    if (want < chunk) chunk = want & ~(MiB - 1);
    if (chunk < 16 * MiB) { stab_fail_setup(STAB_VRAM, "not enough free video memory (%llu MiB)", avail / MiB); return; }
    ID3D12Resource* chunks[VRAM_MAX_CHUNKS] = {};
    u32 n = 0;
    stab_phase(STAB_VRAM, "Allocating");
    for (u64 got = 0; got + chunk <= want && n < VRAM_MAX_CHUNKS; got += chunk) {
        chunks[n] = make_buffer(g, chunk, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        if (!chunks[n]) break;
        ++n;
    }
    if (!n) { stab_fail_setup(STAB_VRAM, "allocating %llu MiB of video memory failed", chunk / MiB); return; }
    ts->chunk_count = n;
    ts->chunk_bytes = chunk;
    ts->size_bytes = (u64)n * chunk;
    snprintf(ts->note, sizeof(ts->note), "%llu MiB in %u chunks of %llu MiB (%.0f%% of %llu MiB free)", ts->size_bytes / MiB, n,
             chunk / MiB, avail ? 100.0 * ts->size_bytes / avail : 0.0, avail / MiB);

    const u32 words = (u32)(chunk / 4);
    static const char* const STEP_NAME[5] = { "Address in address", "Walking 1s", "Walking 0s", "Random", "Moving inversions" };
    static const char* const WHAT[2] = { "VRAM pattern", "VRAM pattern" };
    bool injected = !g_stab.cfg.inject;
    for (u32 pass = 0; !stab_done(deadline) && !g->lost; ++pass) {
        for (u32 step = 0; step < 5 && !stab_done(deadline) && !g->lost; ++step) {
            stab_phase(STAB_VRAM, STEP_NAME[step]);
            const u32 kind = step;
            const u32 seed = step == 3 ? (u32)stab_mix(pass) : step == 4 ? ((pass & 1) ? (u32)stab_mix(pass + 7) : 0x55555555u) : pass;
            begin(g);
            g->list->SetPipelineState(g->pso[P_FILL]);
            for (u32 c = 0; c < n; ++c) {
                const u32 inject = !injected && c == 0 ? words / 3 : ~0u;
                const u32 k[8] = { words, kind, seed, c, 0, inject, 0, FILL_THREADS };
                set_uavs(g, chunks[c], nullptr);
                set_constants(g, false, k);
                g->list->Dispatch(FILL_THREADS / 256, 1, 1);
            }
            injected = true;
            uav_barrier(g->list);
            g->list->SetPipelineState(g->pso[P_CHECK]);
            // Moving inversions: check P / write ~P, check ~P / write P, check P. Others: one check.
            const u32 checks = step == 4 ? 3 : 1;
            for (u32 k_i = 0; k_i < checks; ++k_i) {
                const u32 mask = k_i == 1 ? ~0u : 0u;
                const u32 rewrite = step == 4 && k_i < 2;
                for (u32 c = 0; c < n; ++c) {
                    const u32 k[8] = { words, kind, seed, c, mask, ~0u, rewrite, FILL_THREADS };
                    set_uavs(g, chunks[c], nullptr);
                    set_constants(g, false, k);
                    g->list->Dispatch(FILL_THREADS / 256, 1, 1);
                }
                uav_barrier(g->list);
            }
            if (!submit(g, true)) break;
            read_errors(g, WHAT, nullptr);
            stab_work(STAB_VRAM, (f64)checks * ts->size_bytes / 1e9);
        }
    }
    for (u32 c = 0; c < n; ++c) release(chunks[c]);
}

// ---- known-answer compute + render -------------------------------------------------------------------------

static u32 hash_u32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static f32 bits_f32(u32 b) { f32 f; memcpy(&f, &b, 4); return f; }
static u32 f32_bits(f32 f) { u32 b; memcpy(&b, &f, 4); return b; }
static const u32 MUL_BITS[4] = { 0x406CCCCDu, 0x40533333u, 0x4039999Au, 0x4079999Au };
static const u32 ADD_BITS[4] = { 0x3DCCCCCDu, 0x3E4CCCCDu, 0x3E99999Au, 0x3E19999Au };

// One step of the float chain with separately rounded multiply, add, floor and subtract (as `precise` HLSL).
static f32 chain(f32 x, f32 m, f32 a) {
    const __m128 t = _mm_mul_ss(_mm_set_ss(x), _mm_set_ss(m));
    const __m128 u = _mm_add_ss(t, _mm_set_ss(a));
    return _mm_cvtss_f32(_mm_sub_ss(u, _mm_floor_ss(u, u)));
}

static void compute_ref(u32 tid, u32 seed, u32* out) {
    u32 a[4], b[4];
    f32 x[4];
    for (u32 k = 0; k < 4; ++k) {
        a[k] = hash_u32(tid * 4u + k + seed * 0x9E3779B9u);
        b[k] = hash_u32(a[k] ^ 0x85EBCA6Bu);
        x[k] = (f32)(b[k] & 0xFFFFFFu) * (1.0f / 16777216.0f);
    }
    for (u32 i = 0; i < COMPUTE_ITERS; ++i)
        for (u32 k = 0; k < 4; ++k) {
            a[k] = a[k] * 1664525u + (b[k] ^ (a[k] >> 13));
            b[k] = ((b[k] << 7) | (b[k] >> 25)) ^ a[k];
            x[k] = chain(x[k], bits_f32(MUL_BITS[k]), bits_f32(ADD_BITS[k]));
        }
    for (u32 k = 0; k < 4; ++k) out[k] = a[k] ^ (b[k] << 1) ^ f32_bits(x[k]);
}

static u32 render_ref(u32 px, u32 py, u32 seed, u32 rounds) {
    u32 a = hash_u32((px * 0x8DA6B343u) ^ (py * 0xD8163841u) ^ seed);
    u32 b = hash_u32(a ^ 0x85EBCA6Bu);
    f32 f = (f32)(b & 0xFFFFFFu) * (1.0f / 16777216.0f);
    for (u32 i = 0; i < rounds; ++i) {
        a = a * 1664525u + (b ^ (a >> 13));
        b = ((b << 7) | (b >> 25)) ^ a;
        f = chain(f, bits_f32(MUL_BITS[0]), bits_f32(ADD_BITS[0]));
    }
    return a ^ (b << 1) ^ f32_bits(f);
}

struct RefJob { u32* out; };     // COMPUTE_SEEDS x COMPUTE_THREADS x 4

static void ref_worker(u32 index, void* user) {
    RefJob* j = (RefJob*)user;
    const u32 n = g_topo.lp_count;
    for (u32 s = 0; s < COMPUTE_SEEDS; ++s)
        for (u32 t = index; t < COMPUTE_THREADS; t += n) compute_ref(t, s + 1, j->out + ((u64)s * COMPUTE_THREADS + t) * 4);
}

static u32 adapt(u32 count, f64 seconds) {
    if (seconds <= 0.0) return count;
    f64 next = count * BATCH_TARGET_S / seconds;
    if (next > count * 2.0) next = count * 2.0;
    return next < 1.0 ? 1 : next > 512.0 ? 512 : (u32)next;
}

static void gpu_check_run(Gpu* g, f64 deadline) {
    StabTestState* ts = &g_stab.tests[STAB_GPU];
    // CPU references for COMPUTE_SEEDS seeds, uploaded once.
    stab_phase(STAB_GPU, "Computing references");
    const u64 ref_bytes = (u64)COMPUTE_SEEDS * COMPUTE_THREADS * 16;
    RefJob job = { (u32*)VirtualAlloc(nullptr, ref_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) };
    if (!job.out) { stab_fail_setup(STAB_GPU, "out of memory"); return; }
    u32 lps[STAB_MAX_LP];
    const u32 nlp = g_topo.lp_count < STAB_MAX_LP ? g_topo.lp_count : STAB_MAX_LP;
    for (u32 i = 0; i < nlp; ++i) lps[i] = g_topo.order[i];
    stab_run_threads(nlp, lps, ref_worker, &job);

    ID3D12Resource* refs = make_buffer(g, ref_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, true);
    ID3D12Resource* up = make_buffer(g, ref_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    ID3D12Resource* target = nullptr;
    ID3D12Resource* copy = make_buffer(g, (u64)RENDER_W * RENDER_W * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    ID3D12Resource* readback = make_buffer(g, (u64)RENDER_W * RENDER_W * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = rd.Height = RENDER_W;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_UINT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        g->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&target));
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        g->dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
    }
    void* p = nullptr;
    if (!refs || !up || !target || !copy || !readback || !rtv_heap || FAILED(up->Map(0, nullptr, &p))) {
        stab_fail_setup(STAB_GPU, "out of GPU memory");
    } else {
        memcpy(p, job.out, ref_bytes);
        up->Unmap(0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        g->dev->CreateRenderTargetView(target, nullptr, rtv);
        begin(g);
        g->list->CopyResource(refs, up);
        barrier(g->list, refs, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        submit(g, false);
        ts->size_bytes = ref_bytes;
        snprintf(ts->note, sizeof(ts->note), "compute: %u threads x %u iterations, render: %ux%u x %u layers",
                 COMPUTE_THREADS, COMPUTE_ITERS, RENDER_W, RENDER_W, RENDER_INSTANCES);

        static const char* const WHAT[2] = { "compute result", "rendered pixel" };
        u32 dispatches = 4, frames = 2;
        bool inject_compute = g_stab.cfg.inject, inject_render = g_stab.cfg.inject;
        const D3D12_VIEWPORT vp = { 0.0f, 0.0f, (f32)RENDER_W, (f32)RENDER_W, 0.0f, 1.0f };
        const D3D12_RECT sc = { 0, 0, (LONG)RENDER_W, (LONG)RENDER_W };
        for (u32 batch = 0; !stab_done(deadline) && !g->lost; ++batch) {
            // Compute: `dispatches` x the known-answer kernel, each checked on the GPU against the references.
            stab_phase(STAB_GPU, "Compute");
            const u32 seed = batch % COMPUTE_SEEDS;
            f64 t0 = timer_now();
            begin(g);
            g->list->SetPipelineState(g->pso[P_COMPUTE]);
            set_uavs(g, nullptr, refs);
            g->list->SetComputeRootUnorderedAccessView(3, refs->GetGPUVirtualAddress() + (u64)seed * COMPUTE_THREADS * 16);
            for (u32 d = 0; d < dispatches; ++d) {
                const u32 k[8] = { COMPUTE_ITERS, seed + 1, inject_compute && d == 0 ? 1234u : ~0u, 0, 0, 0, 0, 0 };
                set_constants(g, false, k);
                g->list->Dispatch(COMPUTE_THREADS / 256, 1, 1);
            }
            inject_compute = false;
            if (!submit(g, true)) break;
            read_errors(g, WHAT, nullptr);
            stab_work(STAB_GPU, dispatches);
            dispatches = adapt(dispatches, timer_now() - t0);
            if (stab_done(deadline)) break;

            // Render: `frames` x (draw RENDER_INSTANCES full-screen layers, copy, check every pixel on the GPU);
            // the last frame is also read back and spot-checked on the CPU.
            stab_phase(STAB_GPU, "Render");
            t0 = timer_now();
            begin(g);
            u32 frame_seed = 0;
            for (u32 f = 0; f < frames; ++f) {
                frame_seed = (u32)stab_mix((u64)batch * 1000 + f);
                const bool inject = inject_render && f == 0;
                const u32 k[8] = { RENDER_ROUNDS, frame_seed, inject ? 1u : 0u, RENDER_W, RENDER_INSTANCES, 0, 0, 0 };
                g->list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                g->list->RSSetViewports(1, &vp);
                g->list->RSSetScissorRects(1, &sc);
                g->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g->list->SetPipelineState(g->pso[P_RENDER]);
                set_constants(g, true, k);
                g->list->DrawInstanced(3, RENDER_INSTANCES, 0, 0);
                barrier(g->list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier(g->list, copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                dst.pResource = copy;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, RENDER_W, RENDER_W, 1, RENDER_W * 4 };
                src.pResource = target;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                g->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                barrier(g->list, copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                barrier(g->list, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                g->list->SetPipelineState(g->pso[P_RENDER_CHECK]);
                set_uavs(g, copy, refs);
                set_constants(g, false, k);
                g->list->Dispatch(RENDER_W * RENDER_W / 256, 1, 1);
                uav_barrier(g->list);
            }
            inject_render = false;
            barrier(g->list, copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g->list->CopyResource(readback, copy);
            barrier(g->list, copy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!submit(g, true)) break;
            read_errors(g, WHAT, nullptr);
            // CPU spot check of 2048 pixels of the last frame (an independent reference for the GPU's own check).
            u32* px = nullptr;
            const D3D12_RANGE all = { 0, (SIZE_T)RENDER_W * RENDER_W * 4 };
            if (SUCCEEDED(readback->Map(0, &all, (void**)&px))) {
                const u32 last_seed = frame_seed + RENDER_INSTANCES - 1;
                for (u32 s = 0; s < 2048; ++s) {
                    const u32 i = (u32)(stab_mix((u64)batch * 4096 + s) % ((u64)RENDER_W * RENDER_W));
                    const u32 e = render_ref(i % RENDER_W, i / RENDER_W, last_seed, RENDER_ROUNDS);
                    if (px[i] != e) stab_error(STAB_GPU, -1, 1, (u64)i * 4, "rendered pixel (CPU check)", e, px[i]);
                }
                const D3D12_RANGE none = { 0, 0 };
                readback->Unmap(0, &none);
            }
            stab_work(STAB_GPU, frames);
            frames = adapt(frames, timer_now() - t0);
        }
    }
    release(rtv_heap);
    release(readback);
    release(copy);
    release(target);
    release(up);
    release(refs);
    VirtualFree(job.out, 0, MEM_RELEASE);
}

// ---- max power ----------------------------------------------------------------------------------------------

static void power_run(Gpu* g, f64 deadline) {
    StabTestState* ts = &g_stab.tests[STAB_POWER];
    u64 bytes = 256 * MiB;
    const u64 avail = free_vram(g);
    while (bytes > 16 * MiB && avail && bytes > avail / 2) bytes /= 2;
    ID3D12Resource* b = make_buffer(g, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (!b) { stab_fail_setup(STAB_POWER, "out of GPU memory"); return; }
    ts->size_bytes = bytes;
    snprintf(ts->note, sizeof(ts->note), "unchecked load: FMA x %u per element over %llu MiB of VRAM", POWER_ITERS, bytes / MiB);
    stab_phase(STAB_POWER, "Max power");
    static const char* const WHAT[2] = { "power", "power" };
    u32 dispatches = 2;
    while (!stab_done(deadline) && !g->lost) {
        const f64 t0 = timer_now();
        begin(g);
        g->list->SetPipelineState(g->pso[P_POWER]);
        set_uavs(g, b, nullptr);
        const u32 k[8] = { (u32)(bytes / 16), POWER_ITERS, FILL_THREADS, 0, 0, 0, 0, 0 };
        set_constants(g, false, k);
        for (u32 d = 0; d < dispatches; ++d) {
            g->list->Dispatch(FILL_THREADS / 256, 1, 1);
            uav_barrier(g->list);
        }
        if (!submit(g, true)) break;
        read_errors(g, WHAT, nullptr);
        stab_work(STAB_POWER, dispatches);
        dispatches = adapt(dispatches, timer_now() - t0);
    }
    release(b);
}

void stab_gpu_run(StabTest test, f64 deadline) {
    Gpu* g = (Gpu*)VirtualAlloc(nullptr, sizeof(Gpu), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g) { stab_fail_setup(test, "out of memory"); return; }
    char err[160];
    if (!gpu_open(g, test, err, sizeof(err))) {
        stab_fail_setup(test, "%s", err);
    } else {
        if (test == STAB_VRAM) vram_run(g, deadline);
        else if (test == STAB_GPU) gpu_check_run(g, deadline);
        else power_run(g, deadline);
    }
    gpu_close(g);
    VirtualFree(g, 0, MEM_RELEASE);
}
