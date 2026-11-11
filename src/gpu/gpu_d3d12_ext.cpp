// D3D12 extra tests (gpu_ext.h): root signature / dispatch / texture helpers and the compute tests (FP64,
// dp4a, atomics; the matrix test is skipped with the reason). Memory, pipeline and texture tests live in
// gpu_d3d12_memory.cpp, gpu_d3d12_pso.cpp and gpu_d3d12_texture.cpp.
#include "gpu/gpu_d3d12_ext.h"
#include "core/results.h"
#include "core/timer.h"
#include <string.h>

#include "cs_fp64_dxil.h"
#include "cs_dp4a_dxil.h"
#include "cs_atomic_one_dxil.h"
#include "cs_atomic_unique_dxil.h"
#include "cs_atomic_lds_dxil.h"

#define DXIL_BLOB(name) g_##name##_dxil, (u32)sizeof(g_##name##_dxil)

constexpr u32 MAX_TIMES = 1u << 16;
static f64 s_ms[MAX_TIMES];
constexpr f64 BATCH_TARGET_MS = 20.0;

static bool cancelled(const CancelFlag* c) { return c->load(std::memory_order_relaxed); }

// ---- root signature / pipelines ---------------------------------------------------------------------

ID3D12RootSignature* d3x_root(D3d* d) {
    const D3D12_DESCRIPTOR_RANGE_FLAGS vol =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, D3D_SET_UAVS, 0, 0, vol, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D_SET_SRVS, 0, 0, vol, 0 },
    };
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[RP_CONSTANTS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[RP_CONSTANTS].Constants = { 0, 0, sizeof(GpuParams) / 4 };
    params[RP_UAVS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[RP_UAVS].DescriptorTable = { 1, &ranges[0] };
    params[RP_SRVS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[RP_SRVS].DescriptorTable = { 1, &ranges[1] };
    for (u32 i = 0; i < 3; ++i) params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // s0 bilinear (mip point), s1 trilinear, s2..s5 anisotropic 2x..16x; all wrap.
    D3D12_STATIC_SAMPLER_DESC samp[GE_ANISO_MODES] = {};
    for (u32 i = 0; i < GE_ANISO_MODES; ++i) {
        D3D12_STATIC_SAMPLER_DESC* s = &samp[i];
        s->Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : i == 1 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_ANISOTROPIC;
        s->AddressU = s->AddressV = s->AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s->MaxAnisotropy = i < 2 ? 1 : 1u << (i - 1);
        s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s->MaxLOD = D3D12_FLOAT32_MAX;
        s->ShaderRegister = i;
        s->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = { 3, params, GE_ANISO_MODES, samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &err);
    d3d_release(err);
    if (FAILED(hr)) return nullptr;
    ID3D12RootSignature* root = nullptr;
    hr = d->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    blob->Release();
    return SUCCEEDED(hr) ? root : nullptr;
}

ID3D12PipelineState* d3x_compute_pso(D3d* d, ID3D12RootSignature* root, const u8* dxil, u32 size) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root;
    pd.CS = { dxil, size };
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(d->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

D3D_SHADER_MODEL d3x_shader_model(D3d* d) {
    for (u32 sm = D3D_SHADER_MODEL_6_9; sm > D3D_SHADER_MODEL_6_0; --sm) {
        D3D12_FEATURE_DATA_SHADER_MODEL f = { (D3D_SHADER_MODEL)sm };
        if (SUCCEEDED(d->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &f, sizeof(f)))) return f.HighestShaderModel;
    }
    return D3D_SHADER_MODEL_6_0;
}

// ---- dispatch ---------------------------------------------------------------------------------------------

static void record(D3d* d, const D3xDispatch* dp) {
    d->list->SetComputeRootSignature(dp->root);
    d->list->SetPipelineState(dp->pso);
    d3d_bind_compute(d, d->list, dp->set, &dp->pc);
}

i32 d3x_timed(D3d* d, const D3xDispatch* dp, f64 warm, f64 seconds, f64 work, const char* phase, const char* unit,
              f64 unit_div, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1, f64* ms, u32 max) {
    u32 batch = 1, n = 0;
    bool measuring = false;
    live_phase(live, warm > 0.0 ? "Warm-up" : phase, unit);
    const f64 t0 = timer_now();
    f64 t_meas = 0.0;
    while (!cancelled(cancel)) {
        const f64 now = timer_now() - t0;
        if (!measuring && now >= warm) {
            measuring = true;
            t_meas = now;
            live_phase(live, phase, unit);
        }
        if (measuring && (now - t_meas >= seconds || n >= max)) break;
        if (measuring && batch > max - n) batch = max - n;
        d3d_begin(d, 0);
        record(d, dp);
        d->list->EndQuery(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (u32 i = 0; i < batch; ++i) {
            d->list->Dispatch(dp->gx, dp->gy, 1);
            d3d_uav_barrier(d->list);
            d->list->EndQuery(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, i + 1);
        }
        d->list->ResolveQueryData(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, batch + 1, d->ts_readback, 0);
        d3d_wait(&d->direct, d3d_submit(d));
        if (FAILED(d->device->GetDeviceRemovedReason())) return -1;
        f64 sum = 0.0;
        for (u32 i = 0; i < batch; ++i) {
            const f64 t = (f64)(d->ts_data[i + 1] - d->ts_data[i]) * d->ts_ms_per_tick;
            sum += t;
            if (measuring) ms[n++] = t;
        }
        if (sum > 0.0 && work > 0.0) live_sample(live, work * batch / (sum / 1000.0) / unit_div);
        gpu_ext_progress(live, timer_now() - t0, warm + seconds, p0, p1);
        const f64 per = sum > 0.0 ? sum / batch : BATCH_TARGET_MS;
        const u32 next = (u32)(BATCH_TARGET_MS / (per > 1e-3 ? per : 1e-3));
        batch = next < 1 ? 1 : next > D3D_MAX_TS - 1 ? D3D_MAX_TS - 1 : next;
    }
    return (i32)n;
}

void d3x_once(D3d* d, const D3xDispatch* dp) {
    d3d_begin(d, 0);
    record(d, dp);
    d->list->Dispatch(dp->gx, dp->gy, 1);
    d3d_wait(&d->direct, d3d_submit(d));
}

// ---- textures -------------------------------------------------------------------------------------------

ID3D12Resource* d3x_texture(D3d* d, DXGI_FORMAT format, u32 size, u32 mips, const u8* data, const u64* offsets) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = size;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = (u16)mips;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = nullptr;
    if (FAILED(d->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&tex))))
        return nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16];
    u32 rows[16];
    u64 row_bytes[16], total = 0;
    d->device->GetCopyableFootprints(&rd, 0, mips, 0, fp, rows, row_bytes, &total);
    ID3D12Resource* up = d3d_buffer(d, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    u8* ptr = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (!up || FAILED(up->Map(0, &none, (void**)&ptr))) {
        d3d_release(up);
        d3d_release(tex);
        return nullptr;
    }
    for (u32 m = 0; m < mips; ++m)      // rows[] counts block rows for BC formats; row_bytes = packed row size
        for (u32 y = 0; y < rows[m]; ++y)
            memcpy(ptr + fp[m].Offset + (u64)y * fp[m].Footprint.RowPitch, data + offsets[m] + y * row_bytes[m], row_bytes[m]);
    up->Unmap(0, nullptr);
    d3d_begin(d, 0);
    for (u32 m = 0; m < mips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        src.pResource = up;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp[m];
        d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    d3d_barrier(d->list, tex, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d3d_wait(&d->direct, d3d_submit(d));
    up->Release();
    return tex;
}

// ---- compute tests --------------------------------------------------------------------------------------

struct Kernel {
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
    ID3D12Resource* sink;
};

static bool kernel_create(D3d* d, Kernel* k, const u8* dxil, u32 size, u64 sink_bytes) {
    *k = {};
    k->root = d3x_root(d);
    k->pso = k->root ? d3x_compute_pso(d, k->root, dxil, size) : nullptr;
    k->sink = d3d_buffer(d, sink_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COMMON);
    return k->pso && k->sink;
}

static void kernel_destroy(D3d* d, Kernel* k) {
    d3d_wait_idle(d);
    d3d_release(k->sink);
    d3d_release(k->pso);
    d3d_release(k->root);
}

static void alu_test(D3d* d, const u8* dxil, u32 size, u32 iters, f64 work, const char* phase, const char* unit,
                     const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    Kernel k;
    const u64 sink_bytes = (u64)GC_ALU_GROUPS * 256 * 4;
    if (!kernel_create(d, &k, dxil, size, sink_bytes)) {
        res_fail(out, "pipeline / resource creation failed");
        kernel_destroy(d, &k);
        return;
    }
    D3xDispatch dp = { k.root, k.pso, d3d_set_alloc(d), { { iters, 0x9e3779b9u, 0x7f4a7c15u, 0x85ebca6bu }, { 1.0f, 1.1f, 1.2f, 1.3f } },
                       GC_ALU_GROUPS, 1 };
    d3d_set_buffer_uav(d, dp.set, 0, k.sink, sink_bytes);
    const i32 n = d3x_timed(d, &dp, gpu_warmup_seconds(p), p->duration_ms / 1000.0, work, phase, unit, 1e12, live, cancel,
                            0.0f, 1.0f, s_ms, MAX_TIMES);
    if (n < 0) res_fail(out, "GPU device removed");
    else if (!cancelled(cancel)) gpu_report_compute(out, unit, 1e12, work, s_ms, (u32)n);
    kernel_destroy(d, &k);
}

static void atomics_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    struct Variant { const u8* dxil; u32 size; u32 iters; const char* name; };
    const Variant vars[3] = {
        { DXIL_BLOB(cs_atomic_one), GE_ATOM_ITERS_ONE, "One address" },
        { DXIL_BLOB(cs_atomic_unique), GE_ATOM_ITERS_UNIQUE, "Unique addresses" },
        { DXIL_BLOB(cs_atomic_lds), GE_ATOM_ITERS_LDS, "Shared memory (LDS)" },
    };
    const u64 sink_bytes = (u64)GE_ATOM_GROUPS * 256 * 4;
    f64 rate[3] = {};
    for (u32 i = 0; i < 3 && !cancelled(cancel); ++i) {
        Kernel k;
        if (!kernel_create(d, &k, vars[i].dxil, vars[i].size, sink_bytes)) {
            res_fail(out, "pipeline / resource creation failed");
            kernel_destroy(d, &k);
            return;
        }
        D3xDispatch dp = { k.root, k.pso, d3d_set_alloc(d), { { vars[i].iters, 0, 0, 0 }, {} }, GE_ATOM_GROUPS, 1 };
        d3d_set_buffer_uav(d, dp.set, 0, k.sink, sink_bytes);
        const f64 work = (f64)GE_ATOM_GROUPS * 256 * vars[i].iters;
        const i32 n = d3x_timed(d, &dp, i == 0 ? gpu_warmup_seconds(p) : 0.05, p->duration_ms / 1000.0, work, vars[i].name,
                                "Gatomics/s", 1e9, live, cancel, i / 3.0f, (i + 1) / 3.0f, s_ms, MAX_TIMES);
        kernel_destroy(d, &k);
        if (n < 0) { res_fail(out, "GPU device removed"); return; }
        rate[i] = gpu_rate(work, s_ms, (u32)n) / 1e9;
    }
    if (cancelled(cancel)) return;
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < 3; ++i) res_add(out, vars[i].name, "Gatomics/s", rate[i]);
}

// ---- entry ---------------------------------------------------------------------------------------------

bool gpu_d3d12_run_ext(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                       const CancelFlag* cancel) {
    D3d* d = (D3d*)state;
    switch (test) {
    case GT_FP64: case GT_DP4A: case GT_MATRIX: case GT_ATOMICS: case GT_MEM_LATENCY: case GT_MEM_SWEEP: case GT_PSO:
    case GT_TEX_FORMATS: case GT_TEX_ANISO: break;
    default: return false;
    }
    if (FAILED(d->device->GetDeviceRemovedReason())) { res_fail(out, "GPU device was removed"); return true; }
    d3d_wait_idle(d);
    d3d_sets_reset(d);
    switch (test) {
    case GT_FP64: {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
        if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) || !o.DoublePrecisionFloatShaderOps)
            res_skip(out, "GPU/driver lacks double-precision shader ops");
        else alu_test(d, DXIL_BLOB(cs_fp64), GE_FP64_ITERS, GE_FP64_OPS_PER_DISPATCH, "FP64", "TFLOPS", p, live, out, cancel);
        break;
    }
    case GT_DP4A:
        if (d3x_shader_model(d) < D3D_SHADER_MODEL_6_4) res_skip(out, "GPU/driver lacks shader model 6.4 (dot4add_i8packed)");
        else alu_test(d, DXIL_BLOB(cs_dp4a), GC_ALU_ITERS, GE_DP4A_OPS_PER_DISPATCH, "INT8 dot4", "TOPS", p, live, out, cancel);
        break;
    case GT_MATRIX: {
        // WaveMMA (OPTIONS9) was never exposed by a released HLSL compiler; SM 6.9 cooperative vectors need the
        // preview Agility SDK runtime, which this app doesn't ship.
        D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9 = {};
        d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9));
        res_skip(out, "no matrix-multiply HLSL in the released D3D12 runtime / DXC (driver WaveMMA tier %u); Vulkan only",
                 (u32)o9.WaveMMATier);
        break;
    }
    case GT_ATOMICS: atomics_test(d, p, live, out, cancel); break;
    case GT_MEM_LATENCY: case GT_MEM_SWEEP: d3x_memory_test(d, test, p, live, out, cancel); break;
    case GT_PSO: d3x_pso_test(d, p, live, out, cancel); break;
    default: d3x_texture_test(d, test, p, live, out, cancel); break;
    }
    d3d_check_device(d, out);
    return true;
}
