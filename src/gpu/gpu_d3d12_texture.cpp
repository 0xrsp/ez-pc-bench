// D3D12 texture sampling (gpu_ext.h): bilinear sampling rate per format and the cost of anisotropic filtering.
#include "gpu/gpu_d3d12_ext.h"
#include "core/results.h"
#include <stdlib.h>

#include "cs_tex_sample_dxil.h"
#include "cs_tex_aniso_dxil.h"

#define DXIL_BLOB(name) g_##name##_dxil, (u32)sizeof(g_##name##_dxil)

constexpr u32 MAX_TIMES = 1u << 15;
static f64 s_ms[MAX_TIMES];

static const DXGI_FORMAT FORMATS[GF_COUNT] = {
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC5_UNORM,
    DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC7_UNORM,
};

struct TexRes {
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
    ID3D12Resource* sink;
    ID3D12Resource* tex;
    u8* data;
    u32 set;
};

static bool tex_setup(D3d* d, TexRes* r, const u8* dxil, u32 size, u64 data_bytes) {
    *r = {};
    r->root = d3x_root(d);
    r->pso = r->root ? d3x_compute_pso(d, r->root, dxil, size) : nullptr;
    r->sink = d3d_buffer(d, 256 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    r->data = (u8*)malloc(data_bytes);
    if (!r->pso || !r->sink || !r->data) return false;
    r->set = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, r->set, 0, r->sink, 256 * 4);
    return true;
}

static void tex_free(D3d* d, TexRes* r) {
    d3d_wait_idle(d);
    free(r->data);
    d3d_release(r->tex);
    d3d_release(r->sink);
    d3d_release(r->pso);
    d3d_release(r->root);
    *r = {};
}

static bool format_usable(D3d* d, DXGI_FORMAT f) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { f };
    return SUCCEEDED(d->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
           (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
}

static void formats_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    TexRes r;
    if (!tex_setup(d, &r, DXIL_BLOB(cs_tex_sample), (u64)GE_TEX_SIZE * GE_TEX_SIZE * 16)) {
        res_fail(out, "pipeline / resource creation failed");
        tex_free(d, &r);
        return;
    }
    f64 seconds = p->duration_ms / 1000.0 / 8.0;
    seconds = seconds < 0.2 ? 0.2 : seconds;
    const f64 work = (f64)GE_TEX_GRID_W * GE_TEX_GRID_H * GE_TEX_SAMPLES;
    f64 rate[GF_COUNT] = {};
    bool ran[GF_COUNT] = {}, ok = true;
    for (u32 f = 0; f < GF_COUNT && ok && !cancel->load(std::memory_order_relaxed); ++f) {
        if (!format_usable(d, FORMATS[f])) continue;
        d3d_wait_idle(d);
        d3d_release(r.tex);
        gpu_ext_tex_fill((GeTexFormat)f, r.data, GE_TEX_SIZE, f + 1);
        const u64 offset = 0;
        r.tex = d3x_texture(d, FORMATS[f], GE_TEX_SIZE, 1, r.data, &offset);
        if (!r.tex) { ok = false; break; }
        d3d_set_texture_srv(d, r.set, 0, r.tex);
        const D3xDispatch dp = { r.root, r.pso, r.set, { { GE_TEX_SAMPLES, 0, 0, 0 }, { 1.0f / GE_TEX_SIZE, 0, 0, 0 } },
                                 GE_TEX_GRID_W / 8, GE_TEX_GRID_H / 8 };
        const i32 n = d3x_timed(d, &dp, f == 0 ? gpu_warmup_seconds(p) : 0.05, seconds, work, GE_TEX_FORMAT_NAMES[f], "Gtex/s",
                                1e9, live, cancel, (f32)f / (f32)GF_COUNT, (f32)(f + 1) / (f32)GF_COUNT, s_ms, MAX_TIMES);
        if (n < 0) { ok = false; break; }
        rate[f] = gpu_rate(work, s_ms, (u32)n) / 1e9;
        ran[f] = true;
    }
    tex_free(d, &r);
    if (!ok) { res_fail(out, "GPU error or texture creation failed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_VALUES);
    for (u32 f = 0; f < GF_COUNT; ++f)
        if (ran[f]) res_add(out, GE_TEX_FORMAT_NAMES[f], "Gtex/s", rate[f]);
}

static void aniso_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    u32 mips = 0;
    u64 offsets[16], bytes = 0;
    for (u32 s = GE_TEX_SIZE; s; s >>= 1) {
        offsets[mips++] = bytes;
        bytes += (u64)s * s * 4;
    }
    TexRes r;
    bool ok = tex_setup(d, &r, DXIL_BLOB(cs_tex_aniso), bytes);
    if (ok) {
        for (u32 i = 0; i < mips; ++i) gpu_ext_tex_fill(GF_RGBA8, r.data + offsets[i], GE_TEX_SIZE >> i, 100 + i);
        r.tex = d3x_texture(d, DXGI_FORMAT_R8G8B8A8_UNORM, GE_TEX_SIZE, mips, r.data, offsets);
        ok = r.tex != nullptr;
    }
    if (!ok) {
        res_fail(out, "pipeline / texture creation failed");
        tex_free(d, &r);
        return;
    }
    d3d_set_texture_srv(d, r.set, 0, r.tex);
    f64 seconds = p->duration_ms / 1000.0 / GE_ANISO_MODES;
    seconds = seconds < 0.2 ? 0.2 : seconds;
    const f64 work = (f64)GE_TEX_GRID_W * GE_TEX_GRID_H * GE_ANISO_SAMPLES;
    const f32 inv = 1.0f / GE_TEX_SIZE;
    f64 rate[GE_ANISO_MODES] = {};
    bool gpu_ok = true;
    for (u32 m = 0; m < GE_ANISO_MODES && !cancel->load(std::memory_order_relaxed); ++m) {
        const D3xDispatch dp = { r.root, r.pso, r.set, { { GE_ANISO_SAMPLES, m, 0, 0 }, { inv, inv * GE_ANISO_RATIO, inv, 0 } },
                                 GE_TEX_GRID_W / 8, GE_TEX_GRID_H / 8 };
        const i32 n = d3x_timed(d, &dp, m == 0 ? gpu_warmup_seconds(p) : 0.05, seconds, work, GE_ANISO_NAMES[m], "Gtex/s", 1e9,
                                live, cancel, (f32)m / GE_ANISO_MODES, (f32)(m + 1) / GE_ANISO_MODES, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        rate[m] = gpu_rate(work, s_ms, (u32)n) / 1e9;
    }
    tex_free(d, &r);
    if (!gpu_ok) { res_fail(out, "GPU device removed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_VALUES);
    for (u32 m = 0; m < GE_ANISO_MODES; ++m) res_add(out, GE_ANISO_NAMES[m], "Gtex/s", rate[m]);
}

void d3x_texture_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                      const CancelFlag* cancel) {
    if (test == GT_TEX_FORMATS) formats_test(d, p, live, out, cancel);
    else aniso_test(d, p, live, out, cancel);
}
