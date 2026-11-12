// D3D12 3D / 2D tests: offscreen frames, frame timing and the in-window preview. Contract: gpu_common.h.
#include "gpu/gpu_d3d12_internal.h"
#include "gpu/gpu_present.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/timer.h"
#include <math.h>
#include <string.h>

constexpr u32 FIF = GPU_FRAMES_IN_FLIGHT;
constexpr f32 BG[4] = { 0.04f, 0.05f, 0.09f, 1.0f };
constexpr u32 RTV_MAIN = 0;

struct Raster {
    u32 w, h;                           // render target
    u32 pw, ph, row_pitch;              // preview
    ID3D12Resource* rt;                 // render target (blit: copy source)
    ID3D12Resource* rt2;                // blit: copy destination
    ID3D12Resource* depth;              // scene
    ID3D12Resource* tex[4];             // texfill: t0..t3, sprites: t0
    ID3D12Resource* preview;            // pw x ph, UAV
    ID3D12Resource* readback[FIF];      // one per frame slot
    const u8* readback_ptr[FIF];
    bool pending[FIF];                  // readback[slot] receives a preview in that slot's frame
    ID3D12Resource* shown;              // what the preview shows (rt or rt2)
    D3D12_RESOURCE_STATES shown_state;  // its state between frames
    u32 set_draw, set_preview;
};

static void raster_release(Raster* r) {
    for (u32 i = 0; i < FIF; ++i) {
        if (r->readback[i] && r->readback_ptr[i]) {
            const D3D12_RANGE none = { 0, 0 };
            r->readback[i]->Unmap(0, &none);
        }
        d3d_release(r->readback[i]);
    }
    d3d_release(r->preview);
    for (u32 i = 0; i < 4; ++i) d3d_release(r->tex[i]);
    d3d_release(r->depth);
    d3d_release(r->rt2);
    d3d_release(r->rt);
}

static u32 hash32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// texfill: 4 RGBA8 noise textures. sprites: one alpha disc.
static bool make_textures(D3d* d, Raster* r, GpuTestId test) {
    const u32 count = test == GT_TEXTURE_FILL ? 4 : 1, size = test == GT_TEXTURE_FILL ? GR_TEX_SIZE : GR_SPRITE_TEX;
    u32* texels = (u32*)big_alloc((u64)size * size * 4);
    if (!texels) return false;
    bool ok = true;
    for (u32 t = 0; t < count && ok; ++t) {
        for (u32 y = 0; y < size; ++y)
            for (u32 x = 0; x < size; ++x) {
                u32 c;
                if (test == GT_TEXTURE_FILL) {
                    c = hash32((y * size + x) * 4 + t) | 0xFF000000u;
                } else {
                    const f32 dx = (x + 0.5f) / size * 2.0f - 1.0f, dy = (y + 0.5f) / size * 2.0f - 1.0f;
                    const f32 a = (1.0f - sqrtf(dx * dx + dy * dy)) * size * 0.5f;  // 1 px soft edge
                    c = 0x00FFFFFFu | ((u32)(a < 0.0f ? 0.0f : a > 1.0f ? 255.0f : a * 255.0f) << 24);
                }
                texels[y * size + x] = c;
            }
        r->tex[t] = d3d_texture(d, size, size, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
        ok = r->tex[t] && d3d_upload_rgba8(d, r->tex[t], (const u8*)texels, size, size,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (ok) d3d_set_texture_srv(d, r->set_draw, t, r->tex[t]);
    }
    big_free(texels);
    return ok;
}

static bool raster_setup(D3d* d, Raster* r, GpuTestId test, const BenchParams* p) {
    r->w = p->gpu_width;
    r->h = p->gpu_height;
    gpu_preview_size(r->w, r->h, &r->pw, &r->ph);
    r->row_pitch = r->pw * 4;   // pw is a multiple of 64 -> 256-byte aligned rows

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    memcpy(cv.Color, BG, sizeof(BG));
    const bool blit = test == GT_BLIT;
    r->rt = d3d_texture(d, r->w, r->h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        blit ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    r->preview = d3d_texture(d, r->pw, r->ph, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
    if (!r->rt || !r->preview) return false;
    d->device->CreateRenderTargetView(r->rt, nullptr, d3d_rtv(d, RTV_MAIN));
    for (u32 i = 0; i < FIF; ++i) {
        r->readback[i] = d3d_buffer(d, (u64)r->row_pitch * r->ph, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        void* ptr = nullptr;
        if (!r->readback[i] || FAILED(r->readback[i]->Map(0, nullptr, &ptr))) return false;
        r->readback_ptr[i] = (const u8*)ptr;
    }
    r->shown = r->rt;
    r->shown_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    r->set_draw = d3d_set_alloc(d);
    r->set_preview = d3d_set_alloc(d);

    if (test == GT_TEXTURE_FILL || test == GT_SPRITES) {
        if (!make_textures(d, r, test)) return false;
    }
    if (test == GT_SCENE) {
        D3D12_CLEAR_VALUE dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.DepthStencil.Depth = 1.0f;
        r->depth = d3d_texture(d, r->w, r->h, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv);
        if (!r->depth) return false;
        d->device->CreateDepthStencilView(r->depth, nullptr, d3d_dsv(d));
    }
    if (blit) {
        // Source = uploaded per-pixel noise (incompressible); frames copy it to rt2, the preview shows rt2.
        r->rt2 = d3d_texture(d, r->w, r->h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_DEST, &cv);
        u32* noise = (u32*)big_alloc((u64)r->w * r->h * 4);
        if (!r->rt2 || !noise) { big_free(noise); return false; }
        for (u32 i = 0; i < r->w * r->h; ++i) noise[i] = hash32(i) | 0xFF000000u;
        const bool ok = d3d_upload_rgba8(d, r->rt, (const u8*)noise, r->w, r->h, D3D12_RESOURCE_STATE_COPY_SOURCE);
        big_free(noise);
        if (!ok) return false;
        r->shown = r->rt2;
        r->shown_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    d3d_set_texture_srv(d, r->set_preview, 0, r->shown);
    d3d_set_texture_uav(d, r->set_preview, 0, r->preview);
    return true;
}

static void record_frame(D3d* d, Raster* r, GpuTestId test, u32 frame, f32 time) {
    ID3D12GraphicsCommandList* list = d->list;
    // Blit / clear: a barrier between consecutive writes to the same target (as the Vulkan backend does).
    if (test == GT_BLIT) {
        for (u32 i = 0; i < GR_BLIT_COPIES; ++i) {
            if (i) d3d_sync_barrier(d, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST);
            list->CopyResource(r->rt2, r->rt);
        }
        return;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d_rtv(d, RTV_MAIN);
    if (test == GT_CLEAR) {
        for (u32 i = 0; i < GR_CLEARS; ++i) {
            const f32 t = (f32)((frame * GR_CLEARS + i) % 1024) / 1024.0f;
            const f32 c[4] = { t, 1.0f - t, 0.5f + 0.5f * (f32)(i & 1), 1.0f };
            if (i) d3d_sync_barrier(d, D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET);
            list->ClearRenderTargetView(rtv, c, 0, nullptr);
        }
        return;
    }

    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, (f32)r->w, (f32)r->h, 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, (LONG)r->w, (LONG)r->h };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = test == GT_SCENE ? d3d_dsv(d) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    list->OMSetRenderTargets(1, &rtv, FALSE, test == GT_SCENE ? &dsv : nullptr);
    list->ClearRenderTargetView(rtv, BG, 0, nullptr);
    if (test == GT_SCENE) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const u32 cols = r->w / 4, rows = r->h / 4;
    GpuParams pc = {};
    switch (test) {
    case GT_TRIANGLES:
        pc = { { cols, rows, 0, 0 }, { 2.0f / cols, 2.0f / rows, 4.0f / r->w, 4.0f / r->h } };
        list->SetPipelineState(d->pipe[DP_TRIANGLES]);
        d3d_bind_graphics(d, list, r->set_draw, &pc);
        list->DrawInstanced(GR_TRI_COUNT * 3, 1, 0, 0);
        break;
    case GT_DRAW_CALLS:
        pc = { { 0, rows, 0, 0 }, { 2.0f / cols, 2.0f / rows, 4.0f / r->w, 4.0f / r->h } };
        list->SetPipelineState(d->pipe[DP_DRAWCALL]);
        d3d_bind_graphics(d, list, r->set_draw, &pc);
        for (u32 i = 0; i < GR_DRAWS; ++i) {
            list->SetGraphicsRoot32BitConstant(RP_CONSTANTS, i, 0);
            list->DrawInstanced(3, 1, 0, 0);
        }
        break;
    case GT_PIXEL_FILL:
        pc.u[0] = frame;
        list->SetPipelineState(d->pipe[DP_FILL]);
        d3d_bind_graphics(d, list, r->set_draw, &pc);
        list->DrawInstanced(3, GR_FILL_LAYERS, 0, 0);
        break;
    case GT_TEXTURE_FILL:
        pc.f[0] = (f32)r->w / GR_TEX_SIZE;
        pc.f[1] = (f32)r->h / GR_TEX_SIZE;
        pc.f[2] = (f32)(frame % 4096) * 0.00137f;
        pc.f[3] = (f32)(frame % 4096) * 0.00071f;
        list->SetPipelineState(d->pipe[DP_TEXFILL]);
        d3d_bind_graphics(d, list, r->set_draw, &pc);
        list->DrawInstanced(3, GR_TEX_LAYERS, 0, 0);
        break;
    case GT_SCENE:
        pc.f[0] = time;
        pc.f[1] = (f32)r->w / r->h;
        list->SetPipelineState(d->pipe[DP_SCENE]);
        d3d_bind_graphics(d, list, r->set_draw, &pc);
        list->DrawInstanced(36, GR_SCENE_CUBES, 0, 0);
        break;
    case GT_SPRITES:
        pc.f[0] = 64.0f / r->w;     // 32 px
        pc.f[1] = 64.0f / r->h;
        pc.f[2] = time;
        list->SetPipelineState(d->pipe[DP_SPRITE]);
        d3d_bind_graphics(d, list, r->set_draw, &pc);
        list->DrawInstanced(6, GR_SPRITES, 0, 0);
        break;
    default: break;
    }
}

// Downscale the shown target into the preview texture and copy it to this slot's readback buffer.
static void record_preview(D3d* d, Raster* r, u32 slot) {
    ID3D12GraphicsCommandList* list = d->list;
    const D3D12_RESOURCE_STATES srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    d3d_barrier(list, r->shown, r->shown_state, srv);
    list->SetPipelineState(d->pipe[DP_DOWNSCALE]);
    const GpuParams pc = { { r->pw, r->ph, 0, 0 }, {} };
    d3d_bind_compute(d, list, r->set_preview, &pc);
    list->Dispatch((r->pw + 7) / 8, (r->ph + 7) / 8, 1);
    d3d_barrier(list, r->preview, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = r->readback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, r->pw, r->ph, 1, r->row_pitch };
    src.pResource = r->preview;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d3d_barrier(list, r->preview, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d3d_barrier(list, r->shown, srv, r->shown_state);
}

static void report(BenchResult* out, GpuTestId test, const Raster* r) {
    const f64 px = (f64)r->w * r->h;
    switch (test) {
    case GT_TRIANGLES: gpu_frames_report(out, GR_TRI_COUNT, "Triangles", "Mtri/s", 1e6); break;
    case GT_PIXEL_FILL: gpu_frames_report(out, px * GR_FILL_LAYERS, "Pixel fill", "Gpix/s", 1e9); break;
    case GT_TEXTURE_FILL: gpu_frames_report(out, px * GR_TEX_LAYERS * 4, "Texel rate", "Gtex/s", 1e9); break;
    case GT_DRAW_CALLS: gpu_frames_report(out, GR_DRAWS, "Draw calls", "K draws/s", 1e3); break;
    case GT_SPRITES: gpu_frames_report(out, GR_SPRITES, "Sprites", "Msprites/s", 1e6); break;
    case GT_BLIT: gpu_frames_report(out, px * 4 * 2 * GR_BLIT_COPIES, "Copy bandwidth", "GB/s", 1e9); break;
    case GT_CLEAR: gpu_frames_report(out, px * 4 * GR_CLEARS, "Clear bandwidth", "GB/s", 1e9); break;
    default: gpu_frames_report(out, 0.0, nullptr, nullptr, 1.0); break;
    }
}

void d3d_raster_test(D3d* d, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                     const CancelFlag* cancel) {
    live_phase(live, "Setup", "");
    Raster r = {};
    if (p->gpu_width == 0 || p->gpu_height == 0) { res_fail(out, "invalid resolution"); return; }
    if (!raster_setup(d, &r, test, p)) {
        d3d_wait_idle(d);
        raster_release(&r);
        res_fail(out, "out of GPU memory");
        return;
    }
    D3dPresent* sc = nullptr;       // on-screen mode (gpu_present.h)
    u32 present_status = 0;
    if (p->gpu_onscreen) {
        char err[128];
        if (!(sc = d3d_present_open(d, p, gpu_test_name(test), err, sizeof(err)))) {
            d3d_wait_idle(d);
            raster_release(&r);
            res_fail(out, "%s", err);
            return;
        }
    }

    // Frame N waits for frame N - FIF, then records and submits; frame time = submit-to-submit (QPC).
    const f64 warmup = gpu_warmup_seconds(p), duration = p->duration_ms / 1000.0;
    u64 fence[FIF] = {};
    u32 frame = 0, live_frames = 0;
    bool measuring = false;
    gpu_frames_begin();
    live_phase(live, "Warm-up", "fps");
    const f64 t_start = timer_now();
    f64 t_prev = t_start, t_measure = t_start, last_sample = t_start, next_preview = t_start;
    while (!cancel->load(std::memory_order_relaxed)) {
        const u32 slot = frame % FIF;
        d3d_wait(&d->direct, fence[slot]);
        if (r.pending[slot]) {
            gpu_preview_publish(r.readback_ptr[slot], r.pw, r.ph, r.row_pitch);
            r.pending[slot] = false;
        }
        d3d_begin(d, slot);
        record_frame(d, &r, test, frame, (f32)(timer_now() - t_start));
        if (timer_now() >= next_preview) {
            record_preview(d, &r, slot);
            r.pending[slot] = true;
            next_preview = timer_now() + GPU_PREVIEW_INTERVAL;
        }
        if (sc) d3d_present_record(sc, d->list, r.shown, r.shown_state);
        fence[slot] = d3d_submit(d);
        if (sc && (present_status = d3d_present_present(sc, measuring)) != 0) break;

        const f64 now = timer_now();
        if (measuring) gpu_frames_push((f32)((now - t_prev) * 1000.0));
        t_prev = now;
        ++frame;
        ++live_frames;
        if (now - last_sample >= 0.05) {
            live_sample(live, live_frames / (now - last_sample));
            last_sample = now;
            live_frames = 0;
        }
        const f64 frac = (now - t_start) / (warmup + duration);
        live_progress(live, (f32)(frac < 1.0 ? frac : 1.0));
        if (!measuring && now - t_start >= warmup) {
            measuring = true;
            t_measure = now;
            live_phase(live, "Measuring", "fps");
            if (sc) d3d_present_begin_measure(sc);
        } else if (measuring && now - t_measure >= duration) {
            break;
        }
    }
    d3d_wait_idle(d);
    d3d_present_close(sc);
    gpu_preview_clear();
    raster_release(&r);
    if (present_status == 1) { res_fail(out, "stopped: the benchmark window was closed"); return; }
    if (present_status == 2) { res_fail(out, "present failed (device removed?)"); return; }
    report(out, test, &r);
    if (sc) present_log_report(out);
}
