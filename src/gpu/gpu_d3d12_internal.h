#pragma once
// D3D12 backend internals shared by gpu_d3d12*.cpp. Contract: gpu_common.h.
#include "gpu/gpu_common.h"
#include <d3d12.h>
#include <dxgi1_6.h>

enum D3dPipe : u8 {
    DP_FP32, DP_FP16, DP_INT32, DP_BW_READ, DP_BW_WRITE, DP_BW_COPY, DP_LDS, DP_DOWNSCALE,
    DP_TRIANGLES, DP_DRAWCALL, DP_FILL, DP_TEXFILL, DP_SCENE, DP_SPRITE,
    DP_COUNT
};

// Root signature parameters (one for everything, see gpu_common.h binding rules).
enum D3dRootParam : u32 { RP_CONSTANTS, RP_UAVS, RP_SRVS };

constexpr u32 D3D_SET_UAVS = 4;                         // u0..u3
constexpr u32 D3D_SET_SRVS = 8;                         // t0..t7
constexpr u32 D3D_SET_SIZE = D3D_SET_UAVS + D3D_SET_SRVS;
constexpr u32 D3D_MAX_SETS = 32;                        // descriptor sets per test
constexpr u32 D3D_MAX_TS = 512;                         // timestamp queries
constexpr u32 D3D_MAX_RTV = 4, D3D_MAX_DSV = 1;

struct GpuParams {                                      // b0, 32 bytes (common.hlsli Params)
    u32 u[4];
    f32 f[4];
};

struct D3dQueue {
    ID3D12CommandQueue* queue;
    ID3D12Fence* fence;
    u64 value;                                          // last signaled
    HANDLE event;
};

struct D3d {
    IDXGIFactory4* factory;
    IDXGIAdapter1* adapter;
    ID3D12Device* device;
    D3dQueue direct, copy;
    ID3D12CommandAllocator* alloc[GPU_FRAMES_IN_FLIGHT];   // direct queue, one per frame slot
    ID3D12GraphicsCommandList* list;
    ID3D12GraphicsCommandList7* list7;                  // same list, enhanced barriers (null if unsupported)
    ID3D12QueryHeap* ts_heap;
    ID3D12Resource* ts_readback;
    const u64* ts_data;                                 // persistently mapped ts_readback
    f64 ts_ms_per_tick;
    ID3D12DescriptorHeap* view_heap;                    // shader visible, D3D_MAX_SETS * D3D_SET_SIZE
    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* dsv_heap;
    u32 view_inc, rtv_inc;
    u32 set_count;                                      // sets allocated in the current test
    ID3D12RootSignature* root;
    ID3D12PipelineState* pipe[DP_COUNT];
    ID3D12Resource* sink;                               // u0 of ALU / LDS shaders (never written in practice)
    bool fp16;                                          // SM 6.2 + native 16-bit ops
};

template <class T> inline void d3d_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// Resources
ID3D12Resource* d3d_buffer(D3d* d, u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES state);
ID3D12Resource* d3d_texture(D3d* d, u32 w, u32 h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear);
// Synchronous upload of tightly packed RGBA8 texels into a texture in COPY_DEST state; leaves it in `after`.
bool d3d_upload_rgba8(D3d* d, ID3D12Resource* tex, const u8* rgba, u32 w, u32 h, D3D12_RESOURCE_STATES after);
void d3d_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                 D3D12_RESOURCE_STATES after);
void d3d_uav_barrier(ID3D12GraphicsCommandList* list);
// Execution + memory dependency without a state change (Vulkan pipeline barrier with the same layout):
// enhanced global barrier sync -> sync, access -> access; falls back to a null aliasing barrier.
void d3d_sync_barrier(D3d* d, D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access);

// Descriptor sets: D3D_SET_SIZE consecutive descriptors (u0..u3, t0..t7), initialized to null views.
void d3d_sets_reset(D3d* d);
u32 d3d_set_alloc(D3d* d);
void d3d_set_buffer_uav(D3d* d, u32 set, u32 slot, ID3D12Resource* res, u64 bytes);   // raw
void d3d_set_buffer_srv(D3d* d, u32 set, u32 slot, ID3D12Resource* res, u64 bytes);   // raw
void d3d_set_texture_uav(D3d* d, u32 set, u32 slot, ID3D12Resource* res);
void d3d_set_texture_srv(D3d* d, u32 set, u32 slot, ID3D12Resource* res);
void d3d_bind_compute(D3d* d, ID3D12GraphicsCommandList* list, u32 set, const GpuParams* pc);
void d3d_bind_graphics(D3d* d, ID3D12GraphicsCommandList* list, u32 set, const GpuParams* pc);
D3D12_CPU_DESCRIPTOR_HANDLE d3d_rtv(D3d* d, u32 index);
D3D12_CPU_DESCRIPTOR_HANDLE d3d_dsv(D3d* d);

// Command submission (direct queue: d->list recorded with d->alloc[slot]).
void d3d_begin(D3d* d, u32 slot);
u64 d3d_submit(D3d* d);                                 // closes d->list, executes, signals
u64 d3d_signal(D3dQueue* q);
void d3d_wait(D3dQueue* q, u64 value);
void d3d_wait_idle(D3d* d);
// After a test: waits idle, reports device removal as a failure, prints debug-layer messages (debug).
bool d3d_check_device(D3d* d, BenchResult* out);

// Tests
void d3d_compute_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                      const CancelFlag* cancel);
void d3d_transfer_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel);
void d3d_raster_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                     const CancelFlag* cancel);

// On-screen mode (gpu_present_d3d12.cpp, see gpu_present.h). One at a time.
struct D3dPresent;
D3dPresent* d3d_present_open(D3d* d, const BenchParams* p, const char* test_name, char* err, u32 err_size);
// Records the copy of `src` (left in src_state) into the current back buffer.
void d3d_present_record(D3dPresent* s, ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state);
u32 d3d_present_present(D3dPresent* s, bool measuring);    // 0 ok, 1 window closed by the user, 2 present failed
void d3d_present_begin_measure(D3dPresent* s);            // the next present starts the display log
void d3d_present_close(D3dPresent* s);
