// D3D12 feature tests (gpu_features.h): DXR 1.1 inline ray tracing, mesh shaders vs the vertex pipeline,
// tessellation, variable-rate shading, sampler feedback and async compute. Offscreen target + preview and
// frame timing as in gpu_d3d12_raster.cpp (contract: gpu_common.h).
#include "gpu/gpu_d3d12_internal.h"
#include "gpu/gpu_features.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/timer.h"
#include <stdlib.h>
#include <string.h>

#include "cs_rt_terrain_dxil.h"
#include "cs_rt_trace_dxil.h"
#include "vs_mesh_dxil.h"
#include "ps_mesh_dxil.h"
#include "as_mesh_dxil.h"
#include "ms_mesh_dxil.h"
#include "vs_tess_dxil.h"
#include "hs_tess_dxil.h"
#include "ds_tess_dxil.h"
#include "ps_tess_dxil.h"
#include "vs_fullscreen_dxil.h"
#include "ps_vrs_dxil.h"
#include "ps_sfb_plain_dxil.h"
#include "ps_sfb_feedback_dxil.h"

#define DXIL(name) D3D12_SHADER_BYTECODE{ g_##name##_dxil, sizeof(g_##name##_dxil) }

constexpr u32 FIF = GPU_FRAMES_IN_FLIGHT;
constexpr u32 TS_PER_SLOT = 16;                 // timestamp queries per frame slot in d->ts_heap
constexpr f32 BG[4] = { 0.04f, 0.05f, 0.09f, 1.0f };

static bool cancelled(const CancelFlag* c) { return c->load(std::memory_order_relaxed); }

static bool shader_model_6_5(D3d* d) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_5 };
    return SUCCEEDED(d->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
           sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
}

static D3D12_CPU_DESCRIPTOR_HANDLE set_cpu(D3d* d, u32 set, u32 slot) {      // slot: u0..u3, then t0..t7
    D3D12_CPU_DESCRIPTOR_HANDLE h = d->view_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (u64)(set * D3D_SET_SIZE + slot) * d->view_inc;
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE set_gpu(D3d* d, u32 set, u32 slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = d->view_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (u64)(set * D3D_SET_SIZE + slot) * d->view_inc;
    return h;
}

static void timestamp(D3d* d, u32 slot, u32 i) { d->list->EndQuery(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT + i); }

static void resolve_timestamps(D3d* d, u32 slot, u32 count) {
    d->list->ResolveQueryData(d->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, slot * TS_PER_SLOT, count, d->ts_readback,
                              (u64)slot * TS_PER_SLOT * 8);
}

static f64 ts_ms(D3d* d, u32 slot, u32 from, u32 to) {
    const u64* t = d->ts_data + slot * TS_PER_SLOT;
    return t[to] > t[from] ? (f64)(t[to] - t[from]) * d->ts_ms_per_tick : 0.0;
}

// ---- resources ------------------------------------------------------------------------------------------

static ID3D12Resource* make_buffer(D3d* d, u64 bytes, D3D12_HEAP_TYPE heap, bool uav, D3D12_RESOURCE_STATES state) {
    return d3d_buffer(d, bytes, heap, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE, state);
}

// Synchronous upload into a DEFAULT buffer (buffers decay to COMMON after the submission).
static bool upload_buffer(D3d* d, ID3D12Resource* dst, const void* data, u64 bytes) {
    ID3D12Resource* up = make_buffer(d, bytes, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* ptr = nullptr;
    if (!up || FAILED(up->Map(0, nullptr, &ptr))) { d3d_release(up); return false; }
    memcpy(ptr, data, bytes);
    up->Unmap(0, nullptr);
    d3d_begin(d, 0);
    d->list->CopyBufferRegion(dst, 0, up, 0, bytes);
    d3d_wait(&d->direct, d3d_submit(d));
    up->Release();
    return true;
}

// Texture with `mips` levels in COPY_DEST state, uploaded from tightly packed levels, left in `after`.
static ID3D12Resource* upload_texture(D3d* d, u32 w, u32 h, u32 mips, DXGI_FORMAT format, u32 texel_bytes,
                                      const u8* data, D3D12_RESOURCE_STATES after) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
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
    ID3D12Resource* up = make_buffer(d, total, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    u8* ptr = nullptr;
    if (!up || FAILED(up->Map(0, nullptr, (void**)&ptr))) {
        d3d_release(up);
        tex->Release();
        return nullptr;
    }
    const u8* src = data;
    for (u32 m = 0; m < mips; ++m) {
        const u64 tight = (u64)(w >> m ? w >> m : 1) * texel_bytes;
        for (u32 r = 0; r < rows[m]; ++r) memcpy(ptr + fp[m].Offset + (u64)r * fp[m].Footprint.RowPitch, src + r * tight, tight);
        src += tight * rows[m];
    }
    up->Unmap(0, nullptr);
    d3d_begin(d, 0);
    for (u32 m = 0; m < mips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
        dl.pResource = tex;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = m;
        sl.pResource = up;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp[m];
        d->list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    d3d_barrier(d->list, tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    d3d_wait(&d->direct, d3d_submit(d));
    up->Release();
    return tex;
}

// ---- pipelines ------------------------------------------------------------------------------------------

static ID3D12PipelineState* make_cs(D3d* d, D3D12_SHADER_BYTECODE cs) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = d->root;
    pd.CS = cs;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(d->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

static D3D12_BLEND_DESC opaque_blend() {
    D3D12_BLEND_DESC b = {};
    D3D12_RENDER_TARGET_BLEND_DESC* rt = &b.RenderTarget[0];
    rt->SrcBlend = rt->SrcBlendAlpha = D3D12_BLEND_ONE;
    rt->DestBlend = rt->DestBlendAlpha = D3D12_BLEND_ZERO;
    rt->BlendOp = rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt->LogicOp = D3D12_LOGIC_OP_NOOP;
    rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return b;
}

static D3D12_RASTERIZER_DESC raster_state(bool cull_back) {
    D3D12_RASTERIZER_DESC r = {};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = cull_back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    r.DepthClipEnable = TRUE;
    return r;
}

static D3D12_DEPTH_STENCIL_DESC depth_state(bool depth) {
    D3D12_DEPTH_STENCIL_DESC s = {};
    s.DepthEnable = depth;
    s.DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    s.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    const D3D12_DEPTH_STENCILOP_DESC keep = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                              D3D12_COMPARISON_FUNC_ALWAYS };
    s.FrontFace = s.BackFace = keep;
    return s;
}

struct GfxDesc {
    D3D12_SHADER_BYTECODE vs, hs, ds, ps;
    bool depth, cull_back;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
};

static ID3D12PipelineState* make_gfx(D3d* d, const GfxDesc* g) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = d->root;
    pd.VS = g->vs;
    pd.HS = g->hs;
    pd.DS = g->ds;
    pd.PS = g->ps;
    pd.BlendState = opaque_blend();
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState = raster_state(g->cull_back);
    pd.DepthStencilState = depth_state(g->depth);
    if (g->depth) pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.PrimitiveTopologyType = g->topology;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(d->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

// Pipeline state stream subobject (layout of d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT).
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE T, class D> struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = T;
    D desc;
};

struct MeshStream {
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> raster;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> dsv;
    Sub<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample;
};

static ID3D12PipelineState* make_mesh_pso(D3d* d, ID3D12Device2* dev2) {
    MeshStream s;
    s.root.desc = d->root;
    s.as.desc = DXIL(as_mesh);
    s.ms.desc = DXIL(ms_mesh);
    s.ps.desc = DXIL(ps_mesh);
    s.blend.desc = opaque_blend();
    s.sample_mask.desc = 0xFFFFFFFFu;
    s.raster.desc = raster_state(true);
    s.depth.desc = depth_state(true);
    s.topology.desc = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    s.rtv.desc = {};
    s.rtv.desc.NumRenderTargets = 1;
    s.rtv.desc.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    s.dsv.desc = DXGI_FORMAT_D32_FLOAT;
    s.sample.desc = { 1, 0 };
    const D3D12_PIPELINE_STATE_STREAM_DESC sd = { sizeof(s), &s };
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

// ---- offscreen target, preview and the frame loop ---------------------------------------------------------

struct Target {
    u32 w, h, pw, ph, row_pitch;
    ID3D12Resource* rt;
    D3D12_RESOURCE_STATES rt_state;     // between frames
    ID3D12Resource* depth;
    ID3D12Resource* preview;
    ID3D12Resource* readback[FIF];
    const u8* readback_ptr[FIF];
    bool pending[FIF];
    u32 set_preview;
};

static void target_release(Target* t) {
    for (u32 i = 0; i < FIF; ++i) {
        if (t->readback[i] && t->readback_ptr[i]) {
            const D3D12_RANGE none = { 0, 0 };
            t->readback[i]->Unmap(0, &none);
        }
        d3d_release(t->readback[i]);
    }
    d3d_release(t->preview);
    d3d_release(t->depth);
    d3d_release(t->rt);
}

// uav: the target is written by a compute shader (ray tracing) instead of being rendered to.
static bool target_create(D3d* d, Target* t, const BenchParams* p, bool uav, bool depth) {
    t->w = p->gpu_width < 64 ? 64 : p->gpu_width;
    t->h = p->gpu_height < 64 ? 64 : p->gpu_height;
    gpu_preview_size(t->w, t->h, &t->pw, &t->ph);
    t->row_pitch = t->pw * 4;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    memcpy(cv.Color, BG, sizeof(BG));
    t->rt_state = uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET;
    t->rt = d3d_texture(d, t->w, t->h, DXGI_FORMAT_R8G8B8A8_UNORM,
                        uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        t->rt_state, uav ? nullptr : &cv);
    t->preview = d3d_texture(d, t->pw, t->ph, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    if (!t->rt || !t->preview) return false;
    if (!uav) d->device->CreateRenderTargetView(t->rt, nullptr, d3d_rtv(d, 0));
    if (depth) {
        D3D12_CLEAR_VALUE dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.DepthStencil.Depth = 1.0f;
        t->depth = d3d_texture(d, t->w, t->h, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv);
        if (!t->depth) return false;
        d->device->CreateDepthStencilView(t->depth, nullptr, d3d_dsv(d));
    }
    for (u32 i = 0; i < FIF; ++i) {
        t->readback[i] = make_buffer(d, (u64)t->row_pitch * t->ph, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
        void* ptr = nullptr;
        if (!t->readback[i] || FAILED(t->readback[i]->Map(0, nullptr, &ptr))) return false;
        t->readback_ptr[i] = (const u8*)ptr;
    }
    t->set_preview = d3d_set_alloc(d);
    d3d_set_texture_srv(d, t->set_preview, 0, t->rt);
    d3d_set_texture_uav(d, t->set_preview, 0, t->preview);
    return true;
}

// Binds the target (and depth), clears both, sets viewport + scissor.
static void begin_render(D3d* d, const Target* t) {
    ID3D12GraphicsCommandList* list = d->list;
    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, (f32)t->w, (f32)t->h, 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, (LONG)t->w, (LONG)t->h };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d_rtv(d, 0), dsv = d3d_dsv(d);
    list->OMSetRenderTargets(1, &rtv, FALSE, t->depth ? &dsv : nullptr);
    list->ClearRenderTargetView(rtv, BG, 0, nullptr);
    if (t->depth) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

static void record_preview(D3d* d, Target* t, u32 slot) {
    ID3D12GraphicsCommandList* list = d->list;
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    d3d_barrier(list, t->rt, t->rt_state, srv);
    list->SetPipelineState(d->pipe[DP_DOWNSCALE]);
    const GpuParams pc = { { t->pw, t->ph, 0, 0 }, {} };
    d3d_bind_compute(d, list, t->set_preview, &pc);
    list->Dispatch((t->pw + 7) / 8, (t->ph + 7) / 8, 1);
    d3d_barrier(list, t->preview, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = t->readback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, t->pw, t->ph, 1, t->row_pitch };
    src.pResource = t->preview;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d3d_barrier(list, t->preview, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d3d_barrier(list, t->rt, srv, t->rt_state);
}

typedef void (*RecordFn)(D3d* d, void* ctx, u32 frame, u32 slot, f32 time);
typedef void (*RetireFn)(D3d* d, void* ctx, u32 slot, bool measured);

struct Pass {
    const char* phase;          // live phase while measuring
    f64 warmup, seconds;
    f32 p0, p1;                 // progress range of this pass
    RecordFn record;            // records one frame into d->list (may also submit to other queues)
    RetireFn retire;            // optional: the frame last recorded in `slot` has completed on the direct queue
    void* ctx;
};

// Frame N waits for frame N - FIF, then records and submits; frame time = submit-to-submit (QPC).
// Measured frame times go to feat_pass_*. False if cancelled.
static bool run_pass(D3d* d, Target* t, const Pass* ps, BenchLive* live, const CancelFlag* cancel) {
    u64 fence[FIF] = {};
    bool used[FIF] = {}, measured[FIF] = {};
    u32 frame = 0, live_frames = 0;
    bool measuring = false;
    feat_pass_begin();
    live_phase(live, "Warm-up", "fps");
    const f64 t_start = timer_now();
    f64 t_prev = t_start, t_measure = t_start, last_sample = t_start, next_preview = t_start;
    while (!cancelled(cancel)) {
        const u32 slot = frame % FIF;
        d3d_wait(&d->direct, fence[slot]);
        if (used[slot] && ps->retire) ps->retire(d, ps->ctx, slot, measured[slot]);
        if (t->pending[slot]) {
            gpu_preview_publish(t->readback_ptr[slot], t->pw, t->ph, t->row_pitch);
            t->pending[slot] = false;
        }
        d3d_begin(d, slot);
        ps->record(d, ps->ctx, frame, slot, (f32)(timer_now() - t_start));
        if (timer_now() >= next_preview) {
            record_preview(d, t, slot);
            t->pending[slot] = true;
            next_preview = timer_now() + GPU_PREVIEW_INTERVAL;
        }
        fence[slot] = d3d_submit(d);
        used[slot] = true;
        measured[slot] = measuring;

        const f64 now = timer_now();
        if (measuring) feat_pass_push((f32)((now - t_prev) * 1000.0));
        t_prev = now;
        ++frame;
        ++live_frames;
        if (now - last_sample >= 0.05) {
            live_sample(live, live_frames / (now - last_sample));
            last_sample = now;
            live_frames = 0;
        }
        const f64 frac = (now - t_start) / (ps->warmup + ps->seconds);
        live_progress(live, ps->p0 + (ps->p1 - ps->p0) * (f32)(frac < 1.0 ? frac : 1.0));
        if (!measuring && now - t_start >= ps->warmup) {
            measuring = true;
            t_measure = now;
            live_phase(live, ps->phase, "fps");
        } else if (measuring && now - t_measure >= ps->seconds) {
            break;
        }
    }
    d3d_wait_idle(d);
    for (u32 i = 0; i < FIF; ++i) {
        if (used[i] && ps->retire) ps->retire(d, ps->ctx, i, measured[i]);
        t->pending[i] = false;
    }
    return !cancelled(cancel);
}

static Pass make_pass(const char* phase, const BenchParams* p, u32 index, u32 count, RecordFn record, void* ctx) {
    Pass ps = {};
    ps.phase = phase;
    ps.warmup = feat_pass_warmup(p, index);
    ps.seconds = p->duration_ms / 1000.0;
    ps.p0 = (f32)index / count;
    ps.p1 = (f32)(index + 1) / count;
    ps.record = record;
    ps.ctx = ctx;
    return ps;
}

// ---- ray tracing --------------------------------------------------------------------------------------------

struct Rt {
    ID3D12Device5* dev5;
    ID3D12GraphicsCommandList4* list4;
    ID3D12PipelineState* deform;
    ID3D12PipelineState* trace;
    ID3D12Resource* terrain_vb;
    ID3D12Resource* terrain_ib;
    ID3D12Resource* sphere_vb;
    ID3D12Resource* sphere_ib;
    ID3D12Resource* blas_terrain;
    ID3D12Resource* blas_sphere;
    ID3D12Resource* tlas;
    ID3D12Resource* scratch;            // BLAS builds / refit
    ID3D12Resource* tlas_scratch;
    ID3D12Resource* instances[FIF];     // upload heap, persistently mapped
    FeatInstance* inst_ptr[FIF];
    ID3D12Resource* counter;            // rays traced in the frame (u32)
    ID3D12Resource* counter_rb;         // readback, 8 bytes per slot
    const u32* counter_ptr;
    D3D12_RAYTRACING_GEOMETRY_DESC terrain_geo, sphere_geo;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS terrain_in, sphere_in, tlas_in;
    u32 set_deform, set_trace;
    Target* t;
    u64 rays;                           // measured frames
    f64 refit_ms, tlas_ms, trace_ms;
    u32 timed;
};

static void rt_release(Rt* r) {
    for (u32 i = 0; i < FIF; ++i) {
        if (r->instances[i] && r->inst_ptr[i]) r->instances[i]->Unmap(0, nullptr);
        d3d_release(r->instances[i]);
    }
    if (r->counter_rb && r->counter_ptr) {
        const D3D12_RANGE none = { 0, 0 };
        r->counter_rb->Unmap(0, &none);
    }
    ID3D12Resource** res[] = { &r->terrain_vb, &r->terrain_ib, &r->sphere_vb, &r->sphere_ib, &r->blas_terrain, &r->blas_sphere,
                               &r->tlas, &r->scratch, &r->tlas_scratch, &r->counter, &r->counter_rb };
    for (ID3D12Resource** x : res) d3d_release(*x);
    d3d_release(r->deform);
    d3d_release(r->trace);
    d3d_release(r->list4);
    d3d_release(r->dev5);
}

static ID3D12Resource* make_as(D3d* d, u64 bytes) {
    return make_buffer(d, bytes, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
}

static void build_as(Rt* r, ID3D12Resource* dst, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
                     ID3D12Resource* scratch, bool update) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b = {};
    b.DestAccelerationStructureData = dst->GetGPUVirtualAddress();
    b.Inputs = *in;
    if (update) {
        b.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        b.SourceAccelerationStructureData = b.DestAccelerationStructureData;
    }
    b.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    r->list4->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
}

static void rt_deform(D3d* d, Rt* r, f32 time) {
    const GpuParams pc = { { FR_GRID_VERTS, 0, 0, 0 }, { time, FR_TERRAIN_SIZE / FR_GRID, FR_TERRAIN_SIZE * 0.5f, 0.0f } };
    d->list->SetPipelineState(r->deform);
    d3d_bind_compute(d, d->list, r->set_deform, &pc);
    d->list->Dispatch((FR_GRID_VERTS + 7) / 8, (FR_GRID_VERTS + 7) / 8, 1);
    d3d_uav_barrier(d->list);
    d3d_barrier(d->list, r->terrain_vb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

static bool rt_setup(D3d* d, Rt* r, f64* blas_ms, f64* sphere_blas_ms) {
    if (FAILED(d->device->QueryInterface(IID_PPV_ARGS(&r->dev5))) || FAILED(d->list->QueryInterface(IID_PPV_ARGS(&r->list4))))
        return false;
    r->deform = make_cs(d, DXIL(cs_rt_terrain));
    r->trace = make_cs(d, DXIL(cs_rt_trace));
    if (!r->deform || !r->trace) return false;

    // Geometry: terrain (vertices written by cs_rt_terrain) and the unit sphere.
    const u64 tvb = (u64)FR_TERRAIN_VERTS * 12, tib = (u64)FR_TERRAIN_TRIS * 12;
    const u64 svb = (u64)FR_SPHERE_VERTS * 12, sib = (u64)FR_SPHERE_TRIS * 12;
    r->terrain_vb = make_buffer(d, tvb, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
    r->terrain_ib = make_buffer(d, tib, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
    r->sphere_vb = make_buffer(d, svb, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
    r->sphere_ib = make_buffer(d, sib, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
    r->counter = make_buffer(d, 256, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON);
    r->counter_rb = make_buffer(d, FIF * 8, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!r->terrain_vb || !r->terrain_ib || !r->sphere_vb || !r->sphere_ib || !r->counter || !r->counter_rb) return false;
    if (FAILED(r->counter_rb->Map(0, nullptr, (void**)&r->counter_ptr))) return false;
    u8* mesh = (u8*)malloc(tib + svb + sib);
    if (!mesh) return false;
    feat_terrain_indices((u32*)mesh);
    feat_sphere_mesh((f32*)(mesh + tib), (u32*)(mesh + tib + svb));
    const bool ok = upload_buffer(d, r->terrain_ib, mesh, tib) && upload_buffer(d, r->sphere_vb, mesh + tib, svb) &&
                    upload_buffer(d, r->sphere_ib, mesh + tib + svb, sib);
    free(mesh);
    if (!ok) return false;

    D3D12_RAYTRACING_GEOMETRY_DESC* g = &r->terrain_geo;
    g->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g->Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g->Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    g->Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g->Triangles.IndexCount = FR_TERRAIN_TRIS * 3;
    g->Triangles.VertexCount = FR_TERRAIN_VERTS;
    g->Triangles.IndexBuffer = r->terrain_ib->GetGPUVirtualAddress();
    g->Triangles.VertexBuffer = { r->terrain_vb->GetGPUVirtualAddress(), 12 };
    r->sphere_geo = r->terrain_geo;
    r->sphere_geo.Triangles.IndexCount = FR_SPHERE_TRIS * 3;
    r->sphere_geo.Triangles.VertexCount = FR_SPHERE_VERTS;
    r->sphere_geo.Triangles.IndexBuffer = r->sphere_ib->GetGPUVirtualAddress();
    r->sphere_geo.Triangles.VertexBuffer = { r->sphere_vb->GetGPUVirtualAddress(), 12 };

    const auto fast_trace = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    r->terrain_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    r->terrain_in.Flags = fast_trace | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    r->terrain_in.NumDescs = 1;
    r->terrain_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    r->terrain_in.pGeometryDescs = &r->terrain_geo;
    r->sphere_in = r->terrain_in;
    r->sphere_in.Flags = fast_trace;
    r->sphere_in.pGeometryDescs = &r->sphere_geo;
    r->tlas_in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    r->tlas_in.Flags = fast_trace;
    r->tlas_in.NumDescs = FR_INSTANCES;
    r->tlas_in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti = {}, si = {}, li = {};
    r->dev5->GetRaytracingAccelerationStructurePrebuildInfo(&r->terrain_in, &ti);
    r->dev5->GetRaytracingAccelerationStructurePrebuildInfo(&r->sphere_in, &si);
    r->dev5->GetRaytracingAccelerationStructurePrebuildInfo(&r->tlas_in, &li);
    u64 scratch = ti.ScratchDataSizeInBytes;
    if (ti.UpdateScratchDataSizeInBytes > scratch) scratch = ti.UpdateScratchDataSizeInBytes;
    if (si.ScratchDataSizeInBytes > scratch) scratch = si.ScratchDataSizeInBytes;
    r->blas_terrain = make_as(d, ti.ResultDataMaxSizeInBytes);
    r->blas_sphere = make_as(d, si.ResultDataMaxSizeInBytes);
    r->tlas = make_as(d, li.ResultDataMaxSizeInBytes);
    r->scratch = make_buffer(d, scratch, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    r->tlas_scratch = make_buffer(d, li.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!r->blas_terrain || !r->blas_sphere || !r->tlas || !r->scratch || !r->tlas_scratch) return false;
    for (u32 i = 0; i < FIF; ++i) {
        r->instances[i] = make_buffer(d, sizeof(FeatInstance) * FR_INSTANCES, D3D12_HEAP_TYPE_UPLOAD, false,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!r->instances[i] || FAILED(r->instances[i]->Map(0, nullptr, (void**)&r->inst_ptr[i]))) return false;
    }

    // Descriptor sets: deform (u0 vertices, u1 counter), trace (u0 image, u1 counter, t0 TLAS, t1 / t2 terrain).
    r->set_deform = d3d_set_alloc(d);
    d3d_set_buffer_uav(d, r->set_deform, 0, r->terrain_vb, tvb);
    d3d_set_buffer_uav(d, r->set_deform, 1, r->counter, 256);
    r->set_trace = d3d_set_alloc(d);
    d3d_set_texture_uav(d, r->set_trace, 0, r->t->rt);
    d3d_set_buffer_uav(d, r->set_trace, 1, r->counter, 256);
    D3D12_SHADER_RESOURCE_VIEW_DESC as = {};
    as.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    as.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    as.RaytracingAccelerationStructure.Location = r->tlas->GetGPUVirtualAddress();
    d->device->CreateShaderResourceView(nullptr, &as, set_cpu(d, r->set_trace, D3D_SET_UAVS + 0));
    d3d_set_buffer_srv(d, r->set_trace, 1, r->terrain_ib, tib);
    d3d_set_buffer_srv(d, r->set_trace, 2, r->terrain_vb, tvb);

    // Timed builds: sphere BLAS once, terrain BLAS FR_BLAS_BUILDS times (full builds).
    d3d_begin(d, 0);
    rt_deform(d, r, 0.0f);
    timestamp(d, 0, 0);
    build_as(r, r->blas_sphere, &r->sphere_in, r->scratch, false);
    d3d_uav_barrier(d->list);
    timestamp(d, 0, 1);
    for (u32 i = 0; i < FR_BLAS_BUILDS; ++i) {
        build_as(r, r->blas_terrain, &r->terrain_in, r->scratch, false);
        d3d_uav_barrier(d->list);
        timestamp(d, 0, 2 + i);
    }
    resolve_timestamps(d, 0, 2 + FR_BLAS_BUILDS);
    d3d_wait(&d->direct, d3d_submit(d));
    *sphere_blas_ms = ts_ms(d, 0, 0, 1);
    *blas_ms = ts_ms(d, 0, 1, 1 + FR_BLAS_BUILDS) / FR_BLAS_BUILDS;
    return true;
}

static void rt_record(D3d* d, void* ctx, u32 frame, u32 slot, f32 time) {
    Rt* r = (Rt*)ctx;
    ID3D12GraphicsCommandList* list = d->list;
    timestamp(d, slot, 0);
    rt_deform(d, r, time);                                  // also resets the ray counter
    timestamp(d, slot, 1);
    build_as(r, r->blas_terrain, &r->terrain_in, r->scratch, true);
    d3d_uav_barrier(list);
    timestamp(d, slot, 2);
    feat_rt_instances(r->inst_ptr[slot], time, r->blas_terrain->GetGPUVirtualAddress(), r->blas_sphere->GetGPUVirtualAddress());
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = r->tlas_in;
    in.InstanceDescs = r->instances[slot]->GetGPUVirtualAddress();
    build_as(r, r->tlas, &in, r->tlas_scratch, false);
    d3d_uav_barrier(list);
    timestamp(d, slot, 3);
    const Target* t = r->t;
    const GpuParams pc = { { t->w, t->h, frame, 0 }, { time, (f32)t->w / t->h, 0.0f, 0.0f } };
    list->SetPipelineState(r->trace);
    d3d_bind_compute(d, list, r->set_trace, &pc);
    list->Dispatch((t->w + 7) / 8, (t->h + 7) / 8, 1);
    timestamp(d, slot, 4);
    d3d_uav_barrier(list);
    d3d_barrier(list, r->counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(r->counter_rb, slot * 8, r->counter, 0, 4);
    resolve_timestamps(d, slot, 5);
}

static void rt_retire(D3d* d, void* ctx, u32 slot, bool measured) {
    Rt* r = (Rt*)ctx;
    if (!measured) return;
    r->rays += r->counter_ptr[slot * 2];
    r->refit_ms += ts_ms(d, slot, 1, 2);
    r->tlas_ms += ts_ms(d, slot, 2, 3);
    r->trace_ms += ts_ms(d, slot, 3, 4);
    r->timed++;
}

static void test_raytracing(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) ||
        o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1 || !shader_model_6_5(d)) {
        res_skip(out, "needs DXR 1.1 (raytracing tier 1.1, shader model 6.5)");
        return;
    }
    live_phase(live, "Building BVH", "");
    Target t = {};
    Rt r = {};
    r.t = &t;
    f64 blas_ms = 0.0, sphere_ms = 0.0;
    bool ok = target_create(d, &t, p, true, false) && rt_setup(d, &r, &blas_ms, &sphere_ms);
    if (ok) {
        Pass ps = make_pass("Tracing", p, 0, 1, rt_record, &r);
        ps.retire = rt_retire;
        ok = run_pass(d, &t, &ps, live, cancel);
        const FeatPass fp = feat_pass_end();
        if (ok && fp.frames && r.timed) {
            const f64 seconds = fp.avg_ms * fp.frames / 1000.0, rays_per_frame = (f64)r.rays / r.timed;
            res_begin(out, RESULT_VALUES);
            res_add(out, "Rays", "Grays/s", (f64)r.rays / seconds / 1e9);
            res_add(out, "Rays (trace pass only)", "Grays/s", rays_per_frame / (r.trace_ms / r.timed / 1000.0) / 1e9);
            res_add(out, "Avg frame time", "ms", fp.avg_ms);
            res_add(out, "p99 frame time", "ms", fp.p99_ms);
            res_add(out, "p99.9 frame time", "ms", fp.p999_ms);
            res_add(out, "Trace pass", "ms", r.trace_ms / r.timed);
            res_add(out, "BLAS refit (terrain)", "ms", r.refit_ms / r.timed);
            res_add(out, "TLAS build", "ms", r.tlas_ms / r.timed);
            res_add(out, "BLAS build (terrain)", "ms", blas_ms);
            res_add(out, "BLAS build (sphere)", "ms", sphere_ms);
            res_add(out, "Rays per pixel", "rays", rays_per_frame / ((f64)t.w * t.h));
        }
    } else {
        res_fail(out, "ray tracing setup failed (out of GPU memory?)");
    }
    d3d_wait_idle(d);
    gpu_preview_clear();
    rt_release(&r);
    target_release(&t);
}

// ---- mesh shaders vs vertex pipeline -----------------------------------------------------------------------

struct Mesh {
    ID3D12GraphicsCommandList6* list6;
    ID3D12PipelineState* classic;
    ID3D12PipelineState* mesh;
    ID3D12Resource* ib;
    const Target* t;
    bool use_mesh;
};

static void mesh_record(D3d* d, void* ctx, u32, u32, f32 time) {
    Mesh* m = (Mesh*)ctx;
    ID3D12GraphicsCommandList* list = d->list;
    begin_render(d, m->t);
    const GpuParams pc = { {}, { time, (f32)m->t->w / m->t->h, 0.0f, 0.0f } };
    if (m->use_mesh) {
        list->SetPipelineState(m->mesh);
        d3d_bind_graphics(d, list, 0, &pc);
        m->list6->DispatchMesh(FM_TASK_GROUPS, 1, 1);
    } else {
        list->SetPipelineState(m->classic);
        d3d_bind_graphics(d, list, 0, &pc);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_INDEX_BUFFER_VIEW ibv = { m->ib->GetGPUVirtualAddress(), FM_INDICES * 2, DXGI_FORMAT_R16_UINT };
        list->IASetIndexBuffer(&ibv);
        list->DrawIndexedInstanced(FM_INDICES, FM_OBJECTS, 0, 0, 0);
    }
}

static void test_mesh(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    ID3D12Device2* dev2 = nullptr;
    Mesh m = {};
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))) ||
        o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1 || !shader_model_6_5(d) ||
        FAILED(d->device->QueryInterface(IID_PPV_ARGS(&dev2))) || FAILED(d->list->QueryInterface(IID_PPV_ARGS(&m.list6)))) {
        d3d_release(dev2);
        d3d_release(m.list6);
        res_skip(out, "needs mesh shader tier 1 (shader model 6.5)");
        return;
    }
    live_phase(live, "Setup", "");
    Target t = {};
    m.t = &t;
    const GfxDesc classic = { DXIL(vs_mesh), {}, {}, DXIL(ps_mesh), true, true, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    m.classic = make_gfx(d, &classic);
    m.mesh = make_mesh_pso(d, dev2);
    m.ib = make_buffer(d, FM_INDICES * 2, D3D12_HEAP_TYPE_DEFAULT, false, D3D12_RESOURCE_STATE_COMMON);
    u16* idx = (u16*)malloc(FM_INDICES * 2);
    bool ok = m.classic && m.mesh && m.ib && idx && target_create(d, &t, p, false, true);
    if (ok) {
        feat_mesh_indices(idx);
        ok = upload_buffer(d, m.ib, idx, FM_INDICES * 2);
    }
    free(idx);
    if (!ok) {
        res_fail(out, "mesh shader setup failed");
    } else {
        Pass ps = make_pass("Vertex pipeline", p, 0, 2, mesh_record, &m);
        ok = run_pass(d, &t, &ps, live, cancel);
        const FeatPass classic_pass = feat_pass_end();
        m.use_mesh = true;
        ps = make_pass("Mesh shaders", p, 1, 2, mesh_record, &m);
        ok = ok && run_pass(d, &t, &ps, live, cancel);
        const FeatPass mesh_pass = feat_pass_end();
        if (ok && classic_pass.frames && mesh_pass.frames) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "Vertex pipeline frame time", "ms", classic_pass.avg_ms);
            res_add(out, "Mesh shader frame time", "ms", mesh_pass.avg_ms);
            res_add(out, "Mesh shader speedup", "%", feat_gain_pct(classic_pass.avg_ms, mesh_pass.avg_ms));
            res_add(out, "Vertex pipeline p99 frame time", "ms", classic_pass.p99_ms);
            res_add(out, "Mesh shader p99 frame time", "ms", mesh_pass.p99_ms);
            res_add(out, "Vertex pipeline triangles submitted", "Mtri/s", FM_TRIS_PER_FRAME / classic_pass.avg_ms / 1e3);
        }
    }
    d3d_wait_idle(d);
    gpu_preview_clear();
    target_release(&t);
    d3d_release(m.ib);
    d3d_release(m.mesh);
    d3d_release(m.classic);
    d3d_release(m.list6);
    d3d_release(dev2);
}

// ---- tessellation -----------------------------------------------------------------------------------------

struct Tess {
    ID3D12PipelineState* pso;
    const Target* t;
};

static void tess_record(D3d* d, void* ctx, u32, u32, f32 time) {
    Tess* ts = (Tess*)ctx;
    ID3D12GraphicsCommandList* list = d->list;
    begin_render(d, ts->t);
    const GpuParams pc = { { FT_GRID, 0, 0, 0 }, { time, (f32)ts->t->w / ts->t->h, FT_FACTOR, FT_PATCH_SIZE } };
    list->SetPipelineState(ts->pso);
    d3d_bind_graphics(d, list, 0, &pc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    list->DrawInstanced(FT_GRID * FT_GRID * 4, 1, 0, 0);
}

static void test_tessellation(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Setup", "");
    Target t = {};
    Tess ts = {};
    ts.t = &t;
    const GfxDesc g = { DXIL(vs_tess), DXIL(hs_tess), DXIL(ds_tess), DXIL(ps_tess), true, false,
                        D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH };
    ts.pso = make_gfx(d, &g);
    if (!ts.pso || !target_create(d, &t, p, false, true)) {
        res_fail(out, "tessellation setup failed");
    } else {
        const Pass ps = make_pass("Rendering", p, 0, 1, tess_record, &ts);
        const bool ok = run_pass(d, &t, &ps, live, cancel);
        const FeatPass fp = feat_pass_end();
        if (ok && fp.frames) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "Avg frame time", "ms", fp.avg_ms);
            res_add(out, "p99 frame time", "ms", fp.p99_ms);
            res_add(out, "p99.9 frame time", "ms", fp.p999_ms);
            res_add(out, "Tessellated triangles", "Mtri/s", FT_TRIS_PER_FRAME / fp.avg_ms / 1e3);
        }
    }
    d3d_wait_idle(d);
    gpu_preview_clear();
    target_release(&t);
    d3d_release(ts.pso);
}

// ---- variable-rate shading -----------------------------------------------------------------------------------

enum VrsMode : u8 { VRS_OFF, VRS_2X2, VRS_IMAGE };

struct Vrs {
    ID3D12GraphicsCommandList5* list5;
    ID3D12PipelineState* pso;
    ID3D12Resource* rate_image;
    const Target* t;
    VrsMode mode;
};

static void vrs_record(D3d* d, void* ctx, u32, u32, f32 time) {
    Vrs* v = (Vrs*)ctx;
    ID3D12GraphicsCommandList* list = d->list;
    begin_render(d, v->t);
    if (v->mode == VRS_2X2) {
        v->list5->RSSetShadingRate(D3D12_SHADING_RATE_2X2, nullptr);
    } else if (v->mode == VRS_IMAGE) {
        const D3D12_SHADING_RATE_COMBINER comb[2] = { D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_OVERRIDE };
        v->list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, comb);
        v->list5->RSSetShadingRateImage(v->rate_image);
    }
    const GpuParams pc = { {}, { time, (f32)v->t->w / v->t->h, 0.0f, 0.0f } };
    list->SetPipelineState(v->pso);
    d3d_bind_graphics(d, list, 0, &pc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, FV_LAYERS, 0, 0);
    if (v->mode != VRS_OFF) {
        v->list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
        if (v->mode == VRS_IMAGE) v->list5->RSSetShadingRateImage(nullptr);
    }
}

static void test_vrs(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    Vrs v = {};
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) ||
        o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_1 || FAILED(d->list->QueryInterface(IID_PPV_ARGS(&v.list5)))) {
        res_skip(out, "needs variable-rate shading tier 1");
        return;
    }
    const bool tier2 = o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 && o6.ShadingRateImageTileSize > 0;
    live_phase(live, "Setup", "");
    Target t = {};
    v.t = &t;
    const GfxDesc g = { DXIL(vs_fullscreen), {}, {}, DXIL(ps_vrs), false, false, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    v.pso = make_gfx(d, &g);
    bool ok = v.pso && target_create(d, &t, p, false, false);
    f64 coarse = 0.0;
    if (ok && tier2) {
        const u32 tile = o6.ShadingRateImageTileSize, tw = (t.w + tile - 1) / tile, th = (t.h + tile - 1) / tile;
        u8* rates = (u8*)malloc((u64)tw * th);
        if (rates) {
            coarse = feat_vrs_rate_image(rates, t.w, t.h, tile);
            v.rate_image = upload_texture(d, tw, th, 1, DXGI_FORMAT_R8_UINT, 1, rates, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
            free(rates);
        }
        ok = v.rate_image != nullptr;
    }
    if (!ok) {
        res_fail(out, "VRS setup failed");
    } else {
        const u32 passes = tier2 ? 3 : 2;
        FeatPass fp[3] = {};
        const char* names[3] = { "Shading rate 1x1", "Shading rate 2x2", "Shading rate image" };
        for (u32 i = 0; i < passes && ok; ++i) {
            v.mode = (VrsMode)i;
            const Pass ps = make_pass(names[i], p, i, passes, vrs_record, &v);
            ok = run_pass(d, &t, &ps, live, cancel);
            fp[i] = feat_pass_end();
        }
        if (ok && fp[0].frames && fp[1].frames) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "1x1 frame time", "ms", fp[0].avg_ms);
            res_add(out, "2x2 frame time", "ms", fp[1].avg_ms);
            res_add(out, "2x2 speedup", "%", feat_gain_pct(fp[0].avg_ms, fp[1].avg_ms));
            if (tier2 && fp[2].frames) {
                res_add(out, "Rate image frame time", "ms", fp[2].avg_ms);
                res_add(out, "Rate image speedup", "%", feat_gain_pct(fp[0].avg_ms, fp[2].avg_ms));
                res_add(out, "Rate image coarse pixels", "%", coarse * 100.0);
            }
        }
    }
    d3d_wait_idle(d);
    gpu_preview_clear();
    target_release(&t);
    d3d_release(v.rate_image);
    d3d_release(v.pso);
    d3d_release(v.list5);
}

// ---- sampler feedback ------------------------------------------------------------------------------------------

struct Sfb {
    ID3D12GraphicsCommandList1* list1;
    ID3D12PipelineState* plain;
    ID3D12PipelineState* feedback;
    ID3D12Resource* tex;
    ID3D12Resource* map;            // MinMip feedback map paired with tex
    ID3D12Resource* decoded;        // R8_UINT, one texel per mip region (min mip sampled, 0xFF = none)
    ID3D12DescriptorHeap* cpu_heap; // non-shader-visible copy of the map's UAV (for the clear)
    u32 set;
    const Target* t;
    bool use_feedback;
};

static void sfb_record(D3d* d, void* ctx, u32, u32, f32 time) {
    Sfb* s = (Sfb*)ctx;
    ID3D12GraphicsCommandList* list = d->list;
    if (s->use_feedback) {
        const UINT zero[4] = {};
        list->ClearUnorderedAccessViewUint(set_gpu(d, s->set, 1), s->cpu_heap->GetCPUDescriptorHandleForHeapStart(), s->map, zero, 0, nullptr);
        d3d_uav_barrier(list);
    }
    begin_render(d, s->t);
    const GpuParams pc = { {}, { time, (f32)s->t->w / s->t->h, 0.0f, 0.0f } };
    list->SetPipelineState(s->use_feedback ? s->feedback : s->plain);
    d3d_bind_graphics(d, list, s->set, &pc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, FS_LAYERS, 0, 0);
    if (s->use_feedback) {          // decode the map like a streaming system would, every frame
        d3d_barrier(list, s->map, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        s->list1->ResolveSubresourceRegion(s->decoded, 0, 0, 0, s->map, 0xFFFFFFFFu, nullptr, DXGI_FORMAT_R8_UINT,
                                           D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
        d3d_barrier(list, s->map, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

// Share of mip regions the last decoded feedback map marks as sampled (0..1), -1 on error.
static f64 sfb_touched(D3d* d, Sfb* s) {
    const u32 n = FS_TEX / FS_REGION;
    ID3D12Resource* rb = make_buffer(d, 256ull * n, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rb) return -1.0;
    d3d_begin(d, 0);
    d3d_barrier(d->list, s->decoded, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UINT, n, n, 1, 256 };
    src.pResource = s->decoded;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d3d_barrier(d->list, s->decoded, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    d3d_wait(&d->direct, d3d_submit(d));
    const u8* ptr = nullptr;
    f64 share = -1.0;
    if (SUCCEEDED(rb->Map(0, nullptr, (void**)&ptr))) {
        u32 touched = 0;
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x) touched += ptr[y * 256 + x] != 0xFF;
        share = (f64)touched / (n * n);
        const D3D12_RANGE none = { 0, 0 };
        rb->Unmap(0, &none);
    }
    rb->Release();
    return share;
}

static void test_sampler_feedback(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    ID3D12Device8* dev8 = nullptr;
    Sfb s = {};
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))) ||
        o7.SamplerFeedbackTier < D3D12_SAMPLER_FEEDBACK_TIER_0_9 || !shader_model_6_5(d) ||
        FAILED(d->device->QueryInterface(IID_PPV_ARGS(&dev8))) || FAILED(d->list->QueryInterface(IID_PPV_ARGS(&s.list1)))) {
        d3d_release(dev8);
        d3d_release(s.list1);
        res_skip(out, "needs sampler feedback tier 0.9 (shader model 6.5)");
        return;
    }
    live_phase(live, "Setup", "");
    Target t = {};
    s.t = &t;
    const GfxDesc plain = { DXIL(vs_fullscreen), {}, {}, DXIL(ps_sfb_plain), false, false, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    const GfxDesc fb = { DXIL(vs_fullscreen), {}, {}, DXIL(ps_sfb_feedback), false, false, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    s.plain = make_gfx(d, &plain);
    s.feedback = make_gfx(d, &fb);
    bool ok = s.plain && s.feedback && target_create(d, &t, p, false, false);
    if (ok) {
        u32* texels = (u32*)big_alloc(feat_sfb_texture_bytes());
        if (texels) {
            feat_sfb_texture(texels);
            s.tex = upload_texture(d, FS_TEX, FS_TEX, FS_MIPS, DXGI_FORMAT_R8G8B8A8_UNORM, 4, (const u8*)texels,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            big_free(texels);
        }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC1 md = {};
        md.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        md.Width = FS_TEX;
        md.Height = FS_TEX;
        md.DepthOrArraySize = 1;
        md.MipLevels = FS_MIPS;
        md.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
        md.SampleDesc.Count = 1;
        md.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        md.SamplerFeedbackMipRegion = { FS_REGION, FS_REGION, 1 };
        ok = s.tex && SUCCEEDED(dev8->CreateCommittedResource2(&hp, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               nullptr, nullptr, IID_PPV_ARGS(&s.map)));
        s.decoded = ok ? d3d_texture(d, FS_TEX / FS_REGION, FS_TEX / FS_REGION, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr) : nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        ok = ok && s.decoded && SUCCEEDED(d->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.cpu_heap)));
    }
    if (!ok) {
        res_fail(out, "sampler feedback setup failed");
    } else {
        s.set = d3d_set_alloc(d);
        d3d_set_texture_srv(d, s.set, 0, s.tex);
        dev8->CreateSamplerFeedbackUnorderedAccessView(s.tex, s.map, set_cpu(d, s.set, 1));
        dev8->CreateSamplerFeedbackUnorderedAccessView(s.tex, s.map, s.cpu_heap->GetCPUDescriptorHandleForHeapStart());
        Pass ps = make_pass("Without feedback", p, 0, 2, sfb_record, &s);
        ok = run_pass(d, &t, &ps, live, cancel);
        const FeatPass plain_pass = feat_pass_end();
        s.use_feedback = true;
        ps = make_pass("With feedback", p, 1, 2, sfb_record, &s);
        ok = ok && run_pass(d, &t, &ps, live, cancel);
        const FeatPass fb_pass = feat_pass_end();
        const f64 touched = ok ? sfb_touched(d, &s) : 0.0;
        if (ok && touched <= 0.0) {
            res_fail(out, "the sampler feedback map stayed empty");
        } else if (ok && plain_pass.frames && fb_pass.frames) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "Without feedback frame time", "ms", plain_pass.avg_ms);
            res_add(out, "With feedback frame time", "ms", fb_pass.avg_ms);
            res_add(out, "Feedback overhead", "%", (fb_pass.avg_ms / plain_pass.avg_ms - 1.0) * 100.0);
            res_add(out, "Mip regions sampled", "%", touched * 100.0);
        }
    }
    d3d_wait_idle(d);
    gpu_preview_clear();
    target_release(&t);
    d3d_release(s.cpu_heap);
    d3d_release(s.decoded);
    d3d_release(s.map);
    d3d_release(s.tex);
    d3d_release(s.feedback);
    d3d_release(s.plain);
    d3d_release(s.list1);
    d3d_release(dev8);
}

// ---- async compute ----------------------------------------------------------------------------------------------

struct Async {
    D3dQueue compute;
    ID3D12CommandAllocator* alloc[FIF];
    ID3D12GraphicsCommandList* list;    // compute queue list
    u64 fence[FIF];                     // compute queue value per frame slot
    u32 set_alu;
    f64 iters;                          // cs_fp32 iterations, balanced during the serial warm-up
    f64 gfx_ms, comp_ms;                // measured serial frames: summed GPU time of each part
    u32 timed;
    const Target* t;
    bool async;
};

static void async_graphics(D3d* d, const Target* t, u32 frame) {
    ID3D12GraphicsCommandList* list = d->list;
    begin_render(d, t);
    GpuParams pc = {};
    pc.u[0] = frame;
    list->SetPipelineState(d->pipe[DP_FILL]);
    d3d_bind_graphics(d, list, 0, &pc);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, FA_FILL_LAYERS, 0, 0);
}

static void async_compute(D3d* d, Async* a, ID3D12GraphicsCommandList* list) {
    const GpuParams pc = { { (u32)a->iters, 3, 5, 7 }, { 1.0f, 1.25f, 1.5f, 1.75f } };
    list->SetPipelineState(d->pipe[DP_FP32]);
    d3d_bind_compute(d, list, a->set_alu, &pc);
    list->Dispatch(GC_ALU_GROUPS, 1, 1);
}

static void async_record(D3d* d, void* ctx, u32 frame, u32 slot, f32) {
    Async* a = (Async*)ctx;
    if (!a->async) {        // serial: graphics, full barrier, compute (timestamps balance the two parts)
        timestamp(d, slot, 0);
        async_graphics(d, a->t, frame);
        d3d_sync_barrier(d, D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON);
        timestamp(d, slot, 1);
        async_compute(d, a, d->list);
        timestamp(d, slot, 2);
        resolve_timestamps(d, slot, 3);
        return;
    }
    async_graphics(d, a->t, frame);
    a->alloc[slot]->Reset();
    a->list->Reset(a->alloc[slot], nullptr);
    a->list->SetDescriptorHeaps(1, &d->view_heap);
    a->list->SetComputeRootSignature(d->root);
    async_compute(d, a, a->list);
    a->list->Close();
    ID3D12CommandList* lists[] = { a->list };
    a->compute.queue->ExecuteCommandLists(1, lists);
    a->fence[slot] = d3d_signal(&a->compute);
}

// Async: the frame also waits for its compute work. Serial: during warm-up, scale the compute iterations
// until compute takes as long as graphics; measured frames sum the GPU time of both parts.
static void async_retire(D3d* d, void* ctx, u32 slot, bool measured) {
    Async* a = (Async*)ctx;
    if (a->async) {
        d3d_wait(&a->compute, a->fence[slot]);
        return;
    }
    const f64 g = ts_ms(d, slot, 0, 1), c = ts_ms(d, slot, 1, 2);
    if (measured) {
        a->gfx_ms += g;
        a->comp_ms += c;
        a->timed++;
    } else if (g > 0.0 && c > 0.0) {
        const f64 iters = 0.5 * (a->iters + a->iters * g / c);
        a->iters = iters < 4.0 ? 4.0 : iters > 65536.0 ? 65536.0 : iters;
    }
}

static void test_async(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Setup", "");
    Target t = {};
    Async a = {};
    a.t = &t;
    a.iters = FA_START_ITERS;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    bool ok = SUCCEEDED(d->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&a.compute.queue))) &&
              SUCCEEDED(d->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&a.compute.fence))) &&
              (a.compute.event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr;
    for (u32 i = 0; i < FIF && ok; ++i)
        ok = SUCCEEDED(d->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&a.alloc[i])));
    ok = ok && SUCCEEDED(d->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, a.alloc[0], nullptr, IID_PPV_ARGS(&a.list)));
    if (ok) a.list->Close();
    ok = ok && target_create(d, &t, p, false, false);
    if (!ok) {
        res_fail(out, "async compute setup failed");
    } else {
        a.set_alu = d3d_set_alloc(d);
        d3d_set_buffer_uav(d, a.set_alu, 0, d->sink, (u64)GC_ALU_GROUPS * 256 * 4);
        Pass ps = make_pass("Serial (one queue)", p, 0, 2, async_record, &a);
        ps.retire = async_retire;
        ok = run_pass(d, &t, &ps, live, cancel);
        const FeatPass serial = feat_pass_end();
        a.async = true;
        ps = make_pass("Async (two queues)", p, 1, 2, async_record, &a);
        ps.retire = async_retire;
        ok = ok && run_pass(d, &t, &ps, live, cancel);
        const FeatPass async = feat_pass_end();
        if (ok && serial.frames && async.frames && a.timed) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "Serial frame time", "ms", serial.avg_ms);
            res_add(out, "Async frame time", "ms", async.avg_ms);
            res_add(out, "Overlap gain", "%", feat_gain_pct(serial.avg_ms, async.avg_ms));
            res_add(out, "Graphics part (GPU)", "ms", a.gfx_ms / a.timed);
            res_add(out, "Compute part (GPU)", "ms", a.comp_ms / a.timed);
        }
    }
    d3d_wait_idle(d);
    if (a.compute.fence) d3d_wait(&a.compute, d3d_signal(&a.compute));
    gpu_preview_clear();
    target_release(&t);
    d3d_release(a.list);
    for (u32 i = 0; i < FIF; ++i) d3d_release(a.alloc[i]);
    d3d_release(a.compute.fence);
    d3d_release(a.compute.queue);
    if (a.compute.event) CloseHandle(a.compute.event);
}

// ---- entry ------------------------------------------------------------------------------------------------------

void d3d_feature_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    switch (test) {
    case GT_RAYTRACING:       test_raytracing(d, p, live, out, cancel); break;
    case GT_MESH_SHADER:      test_mesh(d, p, live, out, cancel); break;
    case GT_TESSELLATION:     test_tessellation(d, p, live, out, cancel); break;
    case GT_VRS:              test_vrs(d, p, live, out, cancel); break;
    case GT_SAMPLER_FEEDBACK: test_sampler_feedback(d, p, live, out, cancel); break;
    case GT_ASYNC_COMPUTE:    test_async(d, p, live, out, cancel); break;
    default: break;
    }
}
