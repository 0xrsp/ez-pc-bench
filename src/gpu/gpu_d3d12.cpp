// D3D12 backend for the GPU suite: device, root signature, pipelines and shared helpers.
// Tests live in gpu_d3d12_compute.cpp and gpu_d3d12_raster.cpp. Contract: gpu_common.h.
#include "gpu/gpu_d3d12_internal.h"
#include "gpu/gpu_features.h"
#include "core/results.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_fp32_dxil.h"
#include "cs_fp16_dxil.h"
#include "cs_int32_dxil.h"
#include "cs_bw_read_dxil.h"
#include "cs_bw_write_dxil.h"
#include "cs_bw_copy_dxil.h"
#include "cs_lds_dxil.h"
#include "cs_downscale_dxil.h"
#include "vs_triangles_dxil.h"
#include "vs_drawcall_dxil.h"
#include "ps_color_dxil.h"
#include "vs_fullscreen_dxil.h"
#include "ps_fill_dxil.h"
#include "ps_texfill_dxil.h"
#include "vs_scene_dxil.h"
#include "ps_scene_dxil.h"
#include "vs_sprite_dxil.h"
#include "ps_sprite_dxil.h"

#define DXIL(name) D3D12_SHADER_BYTECODE{ g_##name##_dxil, sizeof(g_##name##_dxil) }

// ---- resources ---------------------------------------------------------------------------------

ID3D12Resource* d3d_buffer(D3d* d, u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES state) {
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
    rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    if (FAILED(d->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

ID3D12Resource* d3d_texture(D3d* d, u32 w, u32 h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    if (FAILED(d->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(&res))))
        return nullptr;
    return res;
}

void d3d_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

void d3d_uav_barrier(ID3D12GraphicsCommandList* list) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    list->ResourceBarrier(1, &b);
}

void d3d_sync_barrier(D3d* d, D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access) {
    if (d->list7) {
        D3D12_GLOBAL_BARRIER g = { sync, sync, access, access };
        D3D12_BARRIER_GROUP group = {};
        group.Type = D3D12_BARRIER_TYPE_GLOBAL;
        group.NumBarriers = 1;
        group.pGlobalBarriers = &g;
        d->list7->Barrier(1, &group);
        return;
    }
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    d->list->ResourceBarrier(1, &b);
}

bool d3d_upload_rgba8(D3d* d, ID3D12Resource* tex, const u8* rgba, u32 w, u32 h, D3D12_RESOURCE_STATES after) {
    const u32 pitch = (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    ID3D12Resource* up = d3d_buffer(d, (u64)pitch * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) return false;
    void* ptr = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (FAILED(up->Map(0, &none, &ptr))) { up->Release(); return false; }
    for (u32 y = 0; y < h; ++y) memcpy((u8*)ptr + (u64)y * pitch, rgba + (u64)y * w * 4, (u64)w * 4);
    up->Unmap(0, nullptr);

    d3d_begin(d, 0);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch };
    d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d3d_barrier(d->list, tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    d3d_wait(&d->direct, d3d_submit(d));
    up->Release();
    return true;
}

// ---- descriptor sets -------------------------------------------------------------------------------

static D3D12_CPU_DESCRIPTOR_HANDLE view_cpu(D3d* d, u32 set, u32 slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = d->view_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (u64)(set * D3D_SET_SIZE + slot) * d->view_inc;
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE view_gpu(D3d* d, u32 set, u32 slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = d->view_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (u64)(set * D3D_SET_SIZE + slot) * d->view_inc;
    return h;
}

void d3d_sets_reset(D3d* d) { d->set_count = 0; }

u32 d3d_set_alloc(D3d* d) {
    const u32 set = d->set_count < D3D_MAX_SETS ? d->set_count++ : D3D_MAX_SETS - 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    for (u32 i = 0; i < D3D_SET_UAVS; ++i) d->device->CreateUnorderedAccessView(nullptr, nullptr, &uav, view_cpu(d, set, i));
    for (u32 i = 0; i < D3D_SET_SRVS; ++i) d->device->CreateShaderResourceView(nullptr, &srv, view_cpu(d, set, D3D_SET_UAVS + i));
    return set;
}

void d3d_set_buffer_uav(D3d* d, u32 set, u32 slot, ID3D12Resource* res, u64 bytes) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    v.Buffer.NumElements = (u32)(bytes / 4);
    v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    d->device->CreateUnorderedAccessView(res, nullptr, &v, view_cpu(d, set, slot));
}

void d3d_set_buffer_srv(D3d* d, u32 set, u32 slot, ID3D12Resource* res, u64 bytes) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Buffer.NumElements = (u32)(bytes / 4);
    v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    d->device->CreateShaderResourceView(res, &v, view_cpu(d, set, D3D_SET_UAVS + slot));
}

void d3d_set_texture_uav(D3d* d, u32 set, u32 slot, ID3D12Resource* res) {
    d->device->CreateUnorderedAccessView(res, nullptr, nullptr, view_cpu(d, set, slot));
}

void d3d_set_texture_srv(D3d* d, u32 set, u32 slot, ID3D12Resource* res) {
    d->device->CreateShaderResourceView(res, nullptr, view_cpu(d, set, D3D_SET_UAVS + slot));
}

void d3d_bind_compute(D3d* d, ID3D12GraphicsCommandList* list, u32 set, const GpuParams* pc) {
    list->SetComputeRoot32BitConstants(RP_CONSTANTS, 8, pc, 0);
    list->SetComputeRootDescriptorTable(RP_UAVS, view_gpu(d, set, 0));
    list->SetComputeRootDescriptorTable(RP_SRVS, view_gpu(d, set, D3D_SET_UAVS));
}

void d3d_bind_graphics(D3d* d, ID3D12GraphicsCommandList* list, u32 set, const GpuParams* pc) {
    list->SetGraphicsRoot32BitConstants(RP_CONSTANTS, 8, pc, 0);
    list->SetGraphicsRootDescriptorTable(RP_UAVS, view_gpu(d, set, 0));
    list->SetGraphicsRootDescriptorTable(RP_SRVS, view_gpu(d, set, D3D_SET_UAVS));
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d_rtv(D3d* d, u32 index) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = d->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (u64)index * d->rtv_inc;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d_dsv(D3d* d) { return d->dsv_heap->GetCPUDescriptorHandleForHeapStart(); }

// ---- submission -------------------------------------------------------------------------------------

void d3d_begin(D3d* d, u32 slot) {
    d->alloc[slot]->Reset();
    d->list->Reset(d->alloc[slot], nullptr);
    d->list->SetDescriptorHeaps(1, &d->view_heap);
    d->list->SetComputeRootSignature(d->root);
    d->list->SetGraphicsRootSignature(d->root);
}

u64 d3d_signal(D3dQueue* q) {
    q->queue->Signal(q->fence, ++q->value);
    return q->value;
}

u64 d3d_submit(D3d* d) {
    d->list->Close();
    ID3D12CommandList* lists[] = { d->list };
    d->direct.queue->ExecuteCommandLists(1, lists);
    return d3d_signal(&d->direct);
}

void d3d_wait(D3dQueue* q, u64 value) {
    if (q->fence->GetCompletedValue() >= value) return;
    q->fence->SetEventOnCompletion(value, q->event);
    WaitForSingleObject(q->event, INFINITE);
}

void d3d_wait_idle(D3d* d) {
    if (d->direct.fence) d3d_wait(&d->direct, d3d_signal(&d->direct));
    if (d->copy.fence) d3d_wait(&d->copy, d3d_signal(&d->copy));
}

static void print_debug_messages(D3d* d) {
#ifndef NDEBUG
    ID3D12InfoQueue* q = nullptr;
    if (FAILED(d->device->QueryInterface(IID_PPV_ARGS(&q)))) return;
    static u64 buf[1024];
    const u64 n = q->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (u64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(q->GetMessage(i, nullptr, &len)) || len > sizeof(buf)) continue;
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)buf;
        if (SUCCEEDED(q->GetMessage(i, m, &len))) fprintf(stderr, "D3D12 debug: %s\n", m->pDescription);
    }
    q->ClearStoredMessages();
    q->Release();
#else
    (void)d;
#endif
}

bool d3d_check_device(D3d* d, BenchResult* out) {
    d3d_wait_idle(d);
    print_debug_messages(d);
    const HRESULT hr = d->device->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        res_fail(out, "GPU device removed (0x%08X)", (u32)hr);
        return false;
    }
    return true;
}

// ---- pipelines ------------------------------------------------------------------------------------

static ID3D12PipelineState* make_compute(D3d* d, D3D12_SHADER_BYTECODE cs) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = d->root;
    pd.CS = cs;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(d->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

enum GfxFlags : u32 { GFX_BLEND = 1u << 0, GFX_DEPTH = 1u << 1 };

static ID3D12PipelineState* make_graphics(D3d* d, D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps, u32 flags) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = d->root;
    pd.VS = vs;
    pd.PS = ps;
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[0];
    b->BlendEnable = (flags & GFX_BLEND) != 0;
    b->SrcBlend = D3D12_BLEND_SRC_ALPHA;
    b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOp = D3D12_BLEND_OP_ADD;
    b->SrcBlendAlpha = D3D12_BLEND_ONE;
    b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b->LogicOp = D3D12_LOGIC_OP_NOOP;
    b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    if (flags & GFX_DEPTH) {
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    }
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(d->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso;
}

static bool make_root_signature(D3d* d) {
    D3D12_FEATURE_DATA_ROOT_SIGNATURE rs = { D3D_ROOT_SIGNATURE_VERSION_1_1 };
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof(rs))) ||
        rs.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1)
        return false;

    // Volatile: sets may contain null views for slots a shader doesn't use.
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

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = { 3, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &err);
    d3d_release(err);
    if (FAILED(hr)) return false;
    hr = d->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&d->root));
    blob->Release();
    return SUCCEEDED(hr);
}

static bool make_pipelines(D3d* d, char* error, u32 error_size) {
    d->pipe[DP_FP32] = make_compute(d, DXIL(cs_fp32));
    d->pipe[DP_INT32] = make_compute(d, DXIL(cs_int32));
    if (d->fp16) d->pipe[DP_FP16] = make_compute(d, DXIL(cs_fp16));
    d->pipe[DP_BW_READ] = make_compute(d, DXIL(cs_bw_read));
    d->pipe[DP_BW_WRITE] = make_compute(d, DXIL(cs_bw_write));
    d->pipe[DP_BW_COPY] = make_compute(d, DXIL(cs_bw_copy));
    d->pipe[DP_LDS] = make_compute(d, DXIL(cs_lds));
    d->pipe[DP_DOWNSCALE] = make_compute(d, DXIL(cs_downscale));
    d->pipe[DP_TRIANGLES] = make_graphics(d, DXIL(vs_triangles), DXIL(ps_color), 0);
    d->pipe[DP_DRAWCALL] = make_graphics(d, DXIL(vs_drawcall), DXIL(ps_color), 0);
    d->pipe[DP_FILL] = make_graphics(d, DXIL(vs_fullscreen), DXIL(ps_fill), 0);
    d->pipe[DP_TEXFILL] = make_graphics(d, DXIL(vs_fullscreen), DXIL(ps_texfill), 0);
    d->pipe[DP_SCENE] = make_graphics(d, DXIL(vs_scene), DXIL(ps_scene), GFX_DEPTH);
    d->pipe[DP_SPRITE] = make_graphics(d, DXIL(vs_sprite), DXIL(ps_sprite), GFX_BLEND);
    for (u32 i = 0; i < DP_COUNT; ++i) {
        if (i == DP_FP16 && !d->fp16) continue;
        if (!d->pipe[i]) {
            snprintf(error, error_size, "creating pipeline %u failed", i);
            return false;
        }
    }
    return true;
}

// ---- suite --------------------------------------------------------------------------------------------

static bool make_queue(D3d* d, D3dQueue* q, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = type;
    if (FAILED(d->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q->queue)))) return false;
    if (FAILED(d->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&q->fence)))) return false;
    q->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return q->event != nullptr;
}

static ID3D12DescriptorHeap* make_heap(D3d* d, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 count, bool shader_visible) {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = type;
    hd.NumDescriptors = count;
    hd.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* h = nullptr;
    if (FAILED(d->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h)))) return nullptr;
    return h;
}

static void enable_debug_layer() {
#ifndef NDEBUG
    ID3D12Debug* dbg = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
        dbg->EnableDebugLayer();
        dbg->Release();
    } else {
        fprintf(stderr, "D3D12 debug layer not available (Windows optional feature \"Graphics Tools\")\n");
    }
#endif
}

static void setup_info_queue(D3d* d) {
#ifndef NDEBUG
    ID3D12InfoQueue* q = nullptr;
    if (FAILED(d->device->QueryInterface(IID_PPV_ARGS(&q)))) return;
    D3D12_MESSAGE_SEVERITY deny_sev[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
    // The clear test changes the clear color on purpose.
    D3D12_MESSAGE_ID deny_id[] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE };
    D3D12_INFO_QUEUE_FILTER f = {};
    f.DenyList.NumSeverities = ARRAY_COUNT(deny_sev);
    f.DenyList.pSeverityList = deny_sev;
    f.DenyList.NumIDs = ARRAY_COUNT(deny_id);
    f.DenyList.pIDList = deny_id;
    q->PushStorageFilter(&f);
    q->Release();
#else
    (void)d;
#endif
}

void gpu_d3d12_shutdown(void* state) {
    D3d* d = (D3d*)state;
    if (!d) return;
    if (d->device) d3d_wait_idle(d);
    if (d->ts_readback && d->ts_data) d->ts_readback->Unmap(0, nullptr);
    d3d_release(d->sink);
    for (u32 i = 0; i < DP_COUNT; ++i) d3d_release(d->pipe[i]);
    d3d_release(d->root);
    d3d_release(d->view_heap);
    d3d_release(d->rtv_heap);
    d3d_release(d->dsv_heap);
    d3d_release(d->ts_readback);
    d3d_release(d->ts_heap);
    d3d_release(d->list7);
    d3d_release(d->list);
    for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i) d3d_release(d->alloc[i]);
    D3dQueue* queues[] = { &d->direct, &d->copy };
    for (D3dQueue* q : queues) {
        d3d_release(q->fence);
        d3d_release(q->queue);
        if (q->event) CloseHandle(q->event);
    }
    d3d_release(d->device);
    d3d_release(d->adapter);
    d3d_release(d->factory);
    free(d);
}

static bool init_fail(D3d* d, char* error, u32 error_size, const char* what, HRESULT hr) {
    if (hr) snprintf(error, error_size, "%s (0x%08X)", what, (u32)hr);
    else snprintf(error, error_size, "%s", what);
    gpu_d3d12_shutdown(d);
    return false;
}

bool gpu_d3d12_init(const BenchParams* p, void** state, char* error, u32 error_size) {
    *state = nullptr;
    const GpuInfo* gi = gpu_selected(p);
    if (!gi) {
        snprintf(error, error_size, "no GPU adapter found");
        return false;
    }
    enable_debug_layer();
    D3d* d = (D3d*)calloc(1, sizeof(D3d));
    if (!d) { snprintf(error, error_size, "out of memory"); return false; }

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&d->factory));
    if (FAILED(hr)) return init_fail(d, error, error_size, "CreateDXGIFactory2 failed", hr);
    LUID luid;
    luid.LowPart = gi->luid_low;
    luid.HighPart = gi->luid_high;
    hr = d->factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&d->adapter));
    if (FAILED(hr)) return init_fail(d, error, error_size, "adapter not found by LUID", hr);
    hr = D3D12CreateDevice(d->adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d->device));
    if (FAILED(hr)) return init_fail(d, error, error_size, "D3D12 feature level 12_0 not supported", hr);
    setup_info_queue(d);

    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_2 };
    if (FAILED(d->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_0)
        return init_fail(d, error, error_size, "shader model 6.0 not supported", 0);
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
    d->fp16 = sm.HighestShaderModel >= D3D_SHADER_MODEL_6_2 &&
              SUCCEEDED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4))) &&
              o4.Native16BitShaderOpsSupported;

    if (!make_queue(d, &d->direct, D3D12_COMMAND_LIST_TYPE_DIRECT) || !make_queue(d, &d->copy, D3D12_COMMAND_LIST_TYPE_COPY))
        return init_fail(d, error, error_size, "creating command queues failed", 0);
    for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i)
        if (FAILED(d->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d->alloc[i]))))
            return init_fail(d, error, error_size, "creating command allocators failed", 0);
    hr = d->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->alloc[0], nullptr, IID_PPV_ARGS(&d->list));
    if (FAILED(hr)) return init_fail(d, error, error_size, "creating command list failed", hr);
    d->list->Close();
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
    if (SUCCEEDED(d->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))) &&
        o12.EnhancedBarriersSupported)
        d->list->QueryInterface(IID_PPV_ARGS(&d->list7));

    // GPU timestamps (direct queue).
    u64 freq = 0;
    if (FAILED(d->direct.queue->GetTimestampFrequency(&freq)) || freq == 0)
        return init_fail(d, error, error_size, "GPU timestamps not supported", 0);
    d->ts_ms_per_tick = 1000.0 / (f64)freq;
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = D3D_MAX_TS;
    hr = d->device->CreateQueryHeap(&qh, IID_PPV_ARGS(&d->ts_heap));
    if (FAILED(hr)) return init_fail(d, error, error_size, "creating timestamp query heap failed", hr);
    d->ts_readback = d3d_buffer(d, D3D_MAX_TS * sizeof(u64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
    void* ts_ptr = nullptr;
    if (!d->ts_readback || FAILED(d->ts_readback->Map(0, nullptr, &ts_ptr)))
        return init_fail(d, error, error_size, "creating timestamp readback buffer failed", 0);
    d->ts_data = (const u64*)ts_ptr;

    d->view_heap = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D_MAX_SETS * D3D_SET_SIZE, true);
    d->rtv_heap = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D_MAX_RTV, false);
    d->dsv_heap = make_heap(d, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D_MAX_DSV, false);
    if (!d->view_heap || !d->rtv_heap || !d->dsv_heap)
        return init_fail(d, error, error_size, "creating descriptor heaps failed", 0);
    d->view_inc = d->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d->rtv_inc = d->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!make_root_signature(d)) return init_fail(d, error, error_size, "creating root signature 1.1 failed", 0);
    if (!make_pipelines(d, error, error_size)) {
        gpu_d3d12_shutdown(d);
        return false;
    }

    d->sink = d3d_buffer(d, (u64)GC_ALU_GROUPS * 256 * 4, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (!d->sink) return init_fail(d, error, error_size, "out of GPU memory", 0);

    *state = d;
    return true;
}

void gpu_d3d12_run(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                   const CancelFlag* cancel) {
    D3d* d = (D3d*)state;
    if (FAILED(d->device->GetDeviceRemovedReason())) { res_fail(out, "GPU device was removed"); return; }
    d3d_wait_idle(d);
    d3d_sets_reset(d);
    switch (test) {
    case GT_FP32: case GT_FP16: case GT_INT32: case GT_VRAM_BW: case GT_LDS_BW:
        d3d_compute_test(d, test, p, live, out, cancel);
        break;
    case GT_TRANSFER:
        d3d_transfer_test(d, p, live, out, cancel);
        break;
    case GT_RAYTRACING: case GT_MESH_SHADER: case GT_TESSELLATION: case GT_VRS: case GT_SAMPLER_FEEDBACK:
    case GT_ASYNC_COMPUTE:
        d3d_feature_test(d, test, p, live, out, cancel);
        break;
    default:
        d3d_raster_test(d, test, p, live, out, cancel);
        break;
    }
    d3d_check_device(d, out);
}
