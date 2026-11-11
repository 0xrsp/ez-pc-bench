// D3D12 on-screen mode (gpu_present.h): flip-model swapchain on the benchmark window, tearing presents,
// display times from IDXGISwapChain::GetFrameStatistics.
#include "gpu/gpu_d3d12_internal.h"
#include "gpu/gpu_present.h"
#include "core/timer.h"
#include <stdio.h>

constexpr u32 BACK_BUFFERS = 3;

struct D3dPresent {
    PresentWindow win;
    IDXGISwapChain3* sc;
    ID3D12Resource* back[BACK_BUFFERS];
    bool tearing;
    i64 last_sync;              // SyncQPCTime of the last frame statistics seen
    f64 qpc_seconds;            // seconds per QPC tick (SyncQPCTime)
};
static D3dPresent s_present;

void d3d_present_close(D3dPresent* s) {
    if (!s) return;
    for (u32 i = 0; i < BACK_BUFFERS; ++i) d3d_release(s->back[i]);
    d3d_release(s->sc);
    present_window_close(&s->win);
    *s = {};
}

D3dPresent* d3d_present_open(D3d* d, const BenchParams* p, const char* test_name, char* err, u32 err_size) {
    D3dPresent* s = &s_present;
    *s = {};
    if (!present_window_open(&s->win, p, "D3D12", test_name, err, err_size)) { d3d_present_close(s); return nullptr; }
    IDXGIFactory5* f5 = nullptr;
    BOOL tearing = FALSE;
    if (SUCCEEDED(d->factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
        if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) tearing = FALSE;
        f5->Release();
    }
    s->tearing = tearing != FALSE;

    // Buffers at the render resolution; DXGI stretches them to the window if the sizes differ.
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = p->gpu_width;
    sd.Height = p->gpu_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = BACK_BUFFERS;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = s->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = d->factory->CreateSwapChainForHwnd(d->direct.queue, s->win.hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        snprintf(err, err_size, "creating the swapchain failed (0x%08X)", (u32)hr);
        d3d_present_close(s);
        return nullptr;
    }
    d->factory->MakeWindowAssociation(s->win.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    hr = sc1->QueryInterface(IID_PPV_ARGS(&s->sc));
    sc1->Release();
    for (u32 i = 0; SUCCEEDED(hr) && i < BACK_BUFFERS; ++i) hr = s->sc->GetBuffer(i, IID_PPV_ARGS(&s->back[i]));
    if (FAILED(hr)) {
        snprintf(err, err_size, "swapchain buffers unavailable (0x%08X)", (u32)hr);
        d3d_present_close(s);
        return nullptr;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s->qpc_seconds = 1.0 / (f64)f.QuadPart;
    return s;
}

static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
    if (before != after) d3d_barrier(list, res, before, after);
}

void d3d_present_record(D3dPresent* s, ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES src_state) {
    ID3D12Resource* back = s->back[s->sc->GetCurrentBackBufferIndex()];
    transition(list, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    d3d_barrier(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(back, src);
    d3d_barrier(list, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
}

// Presents the frame just submitted. `measuring`: log it for the display statistics.
// Returns 0 = ok, 1 = the user closed the window, 2 = present failed (device removed).
u32 d3d_present_present(D3dPresent* s, bool measuring) {
    const HRESULT hr = s->sc->Present(0, s->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    const f64 t = timer_now();
    if (FAILED(hr)) return 2;
    if (measuring) {
        UINT id = 0;
        if (SUCCEEDED(s->sc->GetLastPresentCount(&id))) present_log_presented(id, t);
        DXGI_FRAME_STATISTICS st;
        if (SUCCEEDED(s->sc->GetFrameStatistics(&st)) && st.SyncQPCTime.QuadPart != s->last_sync) {
            s->last_sync = st.SyncQPCTime.QuadPart;
            present_log_displayed(st.PresentCount, (f64)st.SyncQPCTime.QuadPart * s->qpc_seconds);
        }
    }
    return present_window_pump(&s->win) ? 0 : 1;
}

void d3d_present_begin_measure(D3dPresent* s) {
    UINT id = 0;
    s->sc->GetLastPresentCount(&id);
    present_log_begin(id + 1);
}

bool d3d_present_tearing(const D3dPresent* s) { return s->tearing; }
