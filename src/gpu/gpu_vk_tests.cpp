// Vulkan backend: the GPU tests (compute, host <-> GPU transfer, offscreen 3D / 2D with preview).
// Workloads, timing and reporting follow the shared contract in gpu_common.h.
#include "gpu/gpu_vk_internal.h"
#include "core/results.h"
#include "core/timer.h"
#include <string.h>

constexpr u32 MAX_SAMPLES = 1u << 17;
static f64 s_ms[MAX_SAMPLES];   // per-dispatch / per-copy times (ms) of the current phase

static bool cancelled(const CancelFlag* c) { return c->load(std::memory_order_relaxed); }

static bool submit(VkCtx* v, VkQueue queue, VkCommandBuffer cb, VkFence fence) {
    vkResetFences(v->dev, 1, &fence);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    return vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS;
}

static void progress(BenchLive* live, f64 elapsed, f64 total, f32 p0, f32 p1) {
    const f64 frac = total > 0.0 ? elapsed / total : 1.0;
    live_progress(live, p0 + (p1 - p0) * (f32)(frac < 1.0 ? frac : 1.0));
}

// ---- compute -----------------------------------------------------------------------------------------

struct Dispatch {
    VkPipeId pipe;
    VkDescriptorSet set;
    GpuParams pc;
    u32 groups;
};

// Records `count` dispatches, each bracketed by bottom-of-pipe timestamps with a barrier in between,
// submits, waits and stores each dispatch's GPU time in ms[]. false on GPU error.
static bool run_batch(VkCtx* v, const Dispatch* d, u32 count, f64* ms) {
    VkCommandBuffer cb = v->cmd[0];
    if (!fence_wait(v, v->fence[0])) return false;
    cmd_begin(cb);
    vkCmdResetQueryPool(cb, v->query_pool, 0, 2 * count);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipes[d->pipe]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipe_layout[d->pipe], 0, 1, &d->set, 0, nullptr);
    vkCmdPushConstants(cb, v->pipe_layout[d->pipe], VK_PC_STAGES, 0, sizeof(GpuParams), &d->pc);
    for (u32 i = 0; i < count; ++i) {
        if (i) memory_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, v->query_pool, 2 * i);
        vkCmdDispatch(cb, d->groups, 1, 1);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, v->query_pool, 2 * i + 1);
    }
    vkEndCommandBuffer(cb);
    if (!submit(v, v->gfx_queue, cb, v->fence[0]) || !fence_wait(v, v->fence[0])) return false;

    u64 ts[2 * VK_MAX_BATCH];
    if (vkGetQueryPoolResults(v->dev, v->query_pool, 0, 2 * count, sizeof(u64) * 2 * count, ts, sizeof(u64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return false;
    for (u32 i = 0; i < count; ++i) ms[i] = (f64)((ts[2 * i + 1] - ts[2 * i]) & v->ts_mask) * v->ts_period_ns * 1e-6;
    return true;
}

// Warm-up for `warm` seconds, then timed batches (~50 ms each) for `seconds`. Dispatch times land in
// s_ms; returns their count, or -1 on GPU error.
static i32 compute_loop(VkCtx* v, const Dispatch* d, f64 warm, f64 seconds, f64 work, const char* phase,
                        const char* unit, f64 unit_div, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1) {
    f64 batch_ms[VK_MAX_BATCH];
    f64 avg_ms = 0.0, t_meas = 0.0;
    u32 n = 0;
    bool measuring = false;
    live_phase(live, warm > 0.0 ? "Warm-up" : phase, unit);
    const f64 t0 = timer_now();
    while (!cancelled(cancel)) {
        const f64 now = timer_now() - t0;
        if (!measuring && now >= warm) {
            measuring = true;
            t_meas = now;
            live_phase(live, phase, unit);
        }
        if (measuring && (now - t_meas >= seconds || n == MAX_SAMPLES)) break;
        u32 batch = avg_ms > 0.0 ? (u32)(50.0 / avg_ms) : 1;
        batch = batch < 1 ? 1 : batch > VK_MAX_BATCH ? VK_MAX_BATCH : batch;
        if (measuring && batch > MAX_SAMPLES - n) batch = MAX_SAMPLES - n;
        if (!run_batch(v, d, batch, batch_ms)) return -1;
        f64 sum = 0.0;
        for (u32 i = 0; i < batch; ++i) sum += batch_ms[i];
        avg_ms = sum / batch;
        if (measuring) {
            memcpy(s_ms + n, batch_ms, batch * sizeof(f64));
            n += batch;
        }
        if (sum > 0.0) live_sample(live, work * batch / (sum / 1000.0) / unit_div);
        progress(live, timer_now() - t0, warm + seconds, p0, p1);
    }
    return (i32)n;
}

// ALU / LDS kernels: one small output buffer at u0, pc.u.x = iterations.
static void test_kernel(VkCtx* v, VkPipeId pipe, u32 groups, u32 iters, f64 work, const char* phase, const char* unit,
                        f64 unit_div, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    VkScratch s = {};
    VkBuf* ob = scratch_buf(v, &s, (u64)groups * 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_DEVICE);
    const VkDescriptorSet set = ob ? desc_alloc(v, VL_BUF1) : VK_NULL_HANDLE;
    if (!set) {
        res_fail(out, "GPU resource creation failed");
        scratch_free(v, &s);
        return;
    }
    desc_buffer(v, set, 0, ob);
    Dispatch d = { pipe, set, { { iters, 0x9e3779b9u, 0x7f4a7c15u, 0x85ebca6bu }, { 1.0f, 1.1f, 1.2f, 1.3f } }, groups };
    const i32 n = compute_loop(v, &d, gpu_warmup_seconds(p), p->duration_ms / 1000.0, work, phase, unit, unit_div,
                               live, cancel, 0.0f, 1.0f);
    if (n < 0) res_fail(out, "GPU error (device lost or timeout)");
    else if (!cancelled(cancel)) gpu_report_compute(out, unit, unit_div, work, s_ms, (u32)n);
    scratch_free(v, &s);
}

static void test_vram(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    VkScratch s = {};
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuf* a = scratch_buf(v, &s, GC_VRAM_BYTES, usage, MEM_DEVICE);
    VkBuf* b = a ? scratch_buf(v, &s, GC_VRAM_BYTES, usage, MEM_DEVICE) : nullptr;
    const VkDescriptorSet set = b ? desc_alloc(v, VL_BUF2) : VK_NULL_HANDLE;
    if (!set) {
        res_fail(out, "could not allocate 2 x %llu MiB of VRAM", GC_VRAM_BYTES / MiB);
        scratch_free(v, &s);
        return;
    }
    desc_buffer(v, set, 0, b);  // u0 = destination
    desc_buffer(v, set, 8, a);  // t0 = source
    VkCommandBuffer cb = once_begin(v);
    vkCmdFillBuffer(cb, a->buf, 0, VK_WHOLE_SIZE, 0x3c3c3c3cu);
    vkCmdFillBuffer(cb, b->buf, 0, VK_WHOLE_SIZE, 0);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (!once_submit(v)) {
        res_fail(out, "GPU error (device lost or timeout)");
        scratch_free(v, &s);
        return;
    }

    struct Phase { VkPipeId pipe; const char* name; f64 bytes; };
    const Phase phases[3] = {
        { VP_BW_READ, "Read", (f64)GC_VRAM_BYTES },
        { VP_BW_WRITE, "Write", (f64)GC_VRAM_BYTES },
        { VP_BW_COPY, "Copy", 2.0 * GC_VRAM_BYTES },
    };
    f64 rate[3] = {};
    const f64 seconds = p->duration_ms / 1000.0;
    for (u32 i = 0; i < 3; ++i) {
        Dispatch d = { phases[i].pipe, set, { { (u32)(GC_VRAM_BYTES / 16), GC_BW_GROUPS * 256, 0x1234u + i, 0 }, {} }, GC_BW_GROUPS };
        const f64 warm = i == 0 ? gpu_warmup_seconds(p) : 0.05;
        const i32 n = compute_loop(v, &d, warm, seconds, phases[i].bytes, phases[i].name, "GB/s", 1e9, live, cancel,
                                   i / 3.0f, (i + 1) / 3.0f);
        if (n < 0) { res_fail(out, "GPU error (device lost or timeout)"); break; }
        if (cancelled(cancel)) break;
        rate[i] = gpu_rate(phases[i].bytes, s_ms, (u32)n) / 1e9;
        if (i == 2) {
            res_begin(out, RESULT_VALUES);
            for (u32 k = 0; k < 3; ++k) res_add(out, phases[k].name, "GB/s", rate[k]);
        }
    }
    scratch_free(v, &s);
}

// ---- host <-> GPU transfer ----------------------------------------------------------------------------

// Copies src -> dst repeatedly on the transfer queue; wall clock from submit to fence. Returns the
// number of timed copies in s_ms, or -1 on GPU error.
static i32 copy_loop(VkCtx* v, const VkBuf* src, const VkBuf* dst, f64 warm, f64 seconds, const char* phase,
                     BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1) {
    const VkBufferCopy region = { 0, 0, GC_TRANSFER_BYTES };
    u32 n = 0, sample_copies = 0;
    bool measuring = false;
    live_phase(live, warm > 0.0 ? "Warm-up" : phase, "GB/s");
    const f64 t0 = timer_now();
    f64 t_meas = 0.0, t_sample = t0, sample_s = 0.0;
    while (!cancelled(cancel)) {
        const f64 now = timer_now() - t0;
        if (!measuring && now >= warm) {
            measuring = true;
            t_meas = now;
            live_phase(live, phase, "GB/s");
        }
        if (measuring && (now - t_meas >= seconds || n == MAX_SAMPLES)) break;
        if (!fence_wait(v, v->xfer_fence)) return -1;
        cmd_begin(v->xfer_cmd);
        vkCmdCopyBuffer(v->xfer_cmd, src->buf, dst->buf, 1, &region);
        vkEndCommandBuffer(v->xfer_cmd);
        const f64 ts = timer_now();
        if (!submit(v, v->xfer_queue, v->xfer_cmd, v->xfer_fence) || !fence_wait(v, v->xfer_fence)) return -1;
        const f64 te = timer_now();
        if (measuring) s_ms[n++] = (te - ts) * 1000.0;
        sample_s += te - ts;
        ++sample_copies;
        if (te - t_sample >= 0.05) {
            live_sample(live, sample_copies * (f64)GC_TRANSFER_BYTES / sample_s / 1e9);
            t_sample = te;
            sample_s = 0.0;
            sample_copies = 0;
        }
        progress(live, te - t0, warm + seconds, p0, p1);
    }
    return (i32)n;
}

static void test_transfer(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    VkScratch s = {};
    VkBuf* host = scratch_buf(v, &s, GC_TRANSFER_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MEM_UPLOAD);
    VkBuf* dev = host ? scratch_buf(v, &s, GC_TRANSFER_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MEM_DEVICE) : nullptr;
    VkBuf* back = dev ? scratch_buf(v, &s, GC_TRANSFER_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MEM_READBACK) : nullptr;
    if (!back) {
        res_fail(out, "could not allocate transfer buffers");
        scratch_free(v, &s);
        return;
    }
    for (u64 i = 0; i < GC_TRANSFER_BYTES / 4; ++i) ((u32*)host->map)[i] = (u32)(i * 2654435761u);
    const f64 seconds = p->duration_ms / 1000.0;
    const i32 nu = copy_loop(v, host, dev, gpu_warmup_seconds(p), seconds, "Upload", live, cancel, 0.0f, 0.5f);
    const f64 up = nu > 0 ? gpu_rate((f64)GC_TRANSFER_BYTES, s_ms, (u32)nu) / 1e9 : 0.0;
    const i32 nr = nu < 0 || cancelled(cancel) ? 0 : copy_loop(v, dev, back, 0.05, seconds, "Readback", live, cancel, 0.5f, 1.0f);
    const f64 down = nr > 0 ? gpu_rate((f64)GC_TRANSFER_BYTES, s_ms, (u32)nr) / 1e9 : 0.0;
    if (nu < 0 || nr < 0) res_fail(out, "GPU error (device lost or timeout)");
    else if (!cancelled(cancel)) {
        res_begin(out, RESULT_VALUES);
        res_add(out, "Upload", "GB/s", up);
        res_add(out, "Readback", "GB/s", down);
    }
    scratch_free(v, &s);
}

// ---- 3D / 2D frames ------------------------------------------------------------------------------------

static u32 hash_u32(u32 x) {   // same as common.hlsli
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

typedef void (*TexelFn)(u8* dst, u32 w, u32 h, u32 seed);

static void texels_noise(u8* dst, u32 w, u32 h, u32 seed) {
    for (u32 i = 0; i < w * h; ++i) {
        const u32 x = hash_u32(i ^ (seed * 0x9e3779b9u));
        memcpy(dst + (u64)i * 4, &x, 4);
        dst[(u64)i * 4 + 3] = 255;
    }
}

static void texels_disc(u8* dst, u32 w, u32 h, u32) {
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const f32 dx = (x + 0.5f) / w * 2.0f - 1.0f, dy = (y + 0.5f) / h * 2.0f - 1.0f;
            f32 a = (1.0f - (dx * dx + dy * dy)) * 6.0f;     // soft edge
            a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
            u8* t = dst + ((u64)y * w + x) * 4;
            t[0] = t[1] = t[2] = 255;
            t[3] = (u8)(a * 255.0f + 0.5f);
        }
}

// Device-local RGBA8 image filled on the CPU, left in `layout`.
static VkImg* upload_image(VkCtx* v, VkScratch* s, u32 w, u32 h, VkImageUsageFlags usage, TexelFn fill, u32 seed,
                           VkImageLayout layout) {
    VkImg* img = scratch_img(v, s, w, h, VK_RT_FORMAT, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkBuf staging;
    if (!img || !buf_create(v, &staging, (u64)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MEM_UPLOAD)) return nullptr;
    fill(staging.map, w, h, seed);
    VkCommandBuffer cb = once_begin(v);
    image_barrier(cb, img->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { w, h, 1 } };
    vkCmdCopyBufferToImage(cb, staging.buf, img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(cb, img->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, layout);
    const bool ok = once_submit(v);
    buf_destroy(v, &staging);
    return ok ? img : nullptr;
}

struct FrameSetup {
    GpuTestId test;
    u32 w, h, pw, ph;
    VkImg* rt;              // render target (blit: static noise copy source)
    VkImg* rt2;             // blit destination
    VkImg* depth;           // scene
    VkImg* preview;         // cs_downscale output
    VkBuf* readback[GPU_FRAMES_IN_FLIGHT];
    VkDescriptorSet tex_set, down_set;
};

static bool frames_setup(VkCtx* v, VkScratch* s, FrameSetup* f, GpuTestId test, const BenchParams* p) {
    f->test = test;
    f->w = p->gpu_width < 64 ? 64 : p->gpu_width;
    f->h = p->gpu_height < 64 ? 64 : p->gpu_height;
    gpu_preview_size(f->w, f->h, &f->pw, &f->ph);
    const VkImageUsageFlags rt_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (test == GT_BLIT) {
        f->rt = upload_image(v, s, f->w, f->h, rt_usage, texels_noise, 99, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        f->rt2 = scratch_img(v, s, f->w, f->h, VK_RT_FORMAT, rt_usage);
        if (!f->rt2) return false;
    } else {
        f->rt = scratch_img(v, s, f->w, f->h, VK_RT_FORMAT, rt_usage);
    }
    if (!f->rt) return false;
    if (test == GT_SCENE && !(f->depth = scratch_img(v, s, f->w, f->h, VK_DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        return false;
    f->preview = scratch_img(v, s, f->pw, f->ph, VK_RT_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!f->preview) return false;
    for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i)
        if (!(f->readback[i] = scratch_buf(v, s, (u64)f->pw * f->ph * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MEM_READBACK)))
            return false;

    if (test == GT_TEXTURE_FILL || test == GT_SPRITES) {
        if (!(f->tex_set = desc_alloc(v, VL_TEX))) return false;
        for (u32 i = 0; i < 4; ++i) {
            VkImg* t = test == GT_SPRITES
                ? (i == 0 ? upload_image(v, s, GR_SPRITE_TEX, GR_SPRITE_TEX, VK_IMAGE_USAGE_SAMPLED_BIT, texels_disc, 0,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) : &s->imgs[s->nimg - 1])
                : upload_image(v, s, GR_TEX_SIZE, GR_TEX_SIZE, VK_IMAGE_USAGE_SAMPLED_BIT, texels_noise, i + 1,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!t) return false;
            desc_image(v, f->tex_set, 8 + i, t, false);     // sprites: the same texture in t0..t3
        }
    }
    if (!(f->down_set = desc_alloc(v, VL_DOWNSCALE))) return false;
    desc_image(v, f->down_set, 0, f->preview, true);
    desc_image(v, f->down_set, 8, test == GT_BLIT ? f->rt2 : f->rt, false);
    return true;
}

// Everything that may still touch a target from the previous frame (render, copy / clear, preview read).
constexpr VkPipelineStageFlags2 PREV_STAGES = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 PREV_WRITES = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

static void draw_scene(VkCtx* v, const FrameSetup* f, VkCommandBuffer cb, u32 frame, f32 time_s) {
    VkPipeId pipe = VP_TRIANGLES;
    switch (f->test) {
    case GT_TRIANGLES:    pipe = VP_TRIANGLES; break;
    case GT_PIXEL_FILL:   pipe = VP_FILL; break;
    case GT_TEXTURE_FILL: pipe = VP_TEXFILL; break;
    case GT_DRAW_CALLS:   pipe = VP_DRAWCALL; break;
    case GT_SCENE:        pipe = VP_SCENE; break;
    case GT_SPRITES:      pipe = VP_SPRITE; break;
    default: break;
    }
    const VkPipelineLayout layout = v->pipe_layout[pipe];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipes[pipe]);
    if (f->tex_set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &f->tex_set, 0, nullptr);

    const f32 w = (f32)f->w, h = (f32)f->h;
    const u32 cols = f->w / 4, rows = f->h / 4;
    GpuParams pc = {};
    switch (f->test) {
    case GT_TRIANGLES:
    case GT_DRAW_CALLS:
        pc = { { cols, rows, 0, 0 }, { 2.0f / cols, 2.0f / rows, 4.0f / w, 4.0f / h } };
        break;
    case GT_PIXEL_FILL:   pc.u[0] = frame; break;
    case GT_TEXTURE_FILL:
        pc.f[0] = w / GR_TEX_SIZE;
        pc.f[1] = h / GR_TEX_SIZE;
        pc.f[2] = (f32)((frame * 13u) % 10000u) * 1e-4f;
        pc.f[3] = (f32)((frame * 7u) % 10000u) * 1e-4f;
        break;
    case GT_SCENE:        pc.f[0] = time_s; pc.f[1] = w / h; break;
    case GT_SPRITES:      pc.f[0] = 64.0f / w; pc.f[1] = 64.0f / h; pc.f[2] = time_s; break;
    default: break;
    }
    vkCmdPushConstants(cb, layout, VK_PC_STAGES, 0, sizeof(pc), &pc);

    switch (f->test) {
    case GT_TRIANGLES:    vkCmdDraw(cb, GR_TRI_COUNT * 3, 1, 0, 0); break;
    case GT_PIXEL_FILL:   vkCmdDraw(cb, 3, GR_FILL_LAYERS, 0, 0); break;
    case GT_TEXTURE_FILL: vkCmdDraw(cb, 3, GR_TEX_LAYERS, 0, 0); break;
    case GT_SCENE:        vkCmdDraw(cb, 36, GR_SCENE_CUBES, 0, 0); break;
    case GT_SPRITES:      vkCmdDraw(cb, 6, GR_SPRITES, 0, 0); break;
    case GT_DRAW_CALLS:
        for (u32 i = 0; i < GR_DRAWS; ++i) {
            v->cmd_push_constants(cb, layout, VK_PC_STAGES, 0, sizeof(u32), &i);
            v->cmd_draw(cb, 3, 1, 0, 0);
        }
        break;
    default: break;
    }
}

static void record_frame(VkCtx* v, const FrameSetup* f, VkCommandBuffer cb, u32 frame, f32 time_s, bool preview, u32 slot) {
    const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImage src = f->rt->img;               // what the preview samples, and its state after the frame's work
    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2 src_access;
    VkImageLayout src_layout;

    if (f->test == GT_BLIT) {
        // rt holds static noise in TRANSFER_SRC_OPTIMAL since setup. Copies into rt2, ordered by barriers.
        src = f->rt2->img;
        image_barrier(cb, src, color, PREV_STAGES, PREV_WRITES, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageCopy r = { { color, 0, 0, 1 }, { 0, 0, 0 }, { color, 0, 0, 1 }, { 0, 0, 0 }, { f->w, f->h, 1 } };
        for (u32 i = 0; i < GR_BLIT_COPIES; ++i) {
            if (i) image_barrier(cb, src, color, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyImage(cb, f->rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
        src_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
        src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    } else if (f->test == GT_CLEAR) {
        image_barrier(cb, src, color, PREV_STAGES, PREV_WRITES, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range = { color, 0, 1, 0, 1 };
        for (u32 i = 0; i < GR_CLEARS; ++i) {
            if (i) image_barrier(cb, src, color, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const u32 k = frame * GR_CLEARS + i;
            const VkClearColorValue c = { { (k % 64) / 63.0f, (k % 256) / 255.0f, ((k / 64) % 16) / 15.0f, 1.0f } };
            vkCmdClearColorImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        }
        src_stage = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    } else {
        image_barrier(cb, src, color, PREV_STAGES, PREV_WRITES, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        ca.imageView = f->rt->view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = { { 0.02f, 0.02f, 0.05f, 1.0f } };
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { f->w, f->h } };
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        if (f->depth) {
            const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            image_barrier(cb, f->depth->img, VK_IMAGE_ASPECT_DEPTH_BIT, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, tests,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            da.imageView = f->depth->view;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.clearValue.depthStencil = { 1.0f, 0 };
            ri.pDepthAttachment = &da;
        }
        vkCmdBeginRendering(cb, &ri);
        // Negative height flips Y so the image matches D3D12's orientation.
        const VkViewport vp = { 0.0f, (f32)f->h, (f32)f->w, -(f32)f->h, 0.0f, 1.0f };
        const VkRect2D sc = { { 0, 0 }, { f->w, f->h } };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        draw_scene(v, f, cb, frame, time_s);
        vkCmdEndRendering(cb);
        src_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        src_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (!preview) return;

    // Preview: downscale into the storage image, copy it to this slot's readback buffer.
    image_barrier(cb, src, color, src_stage, src_access, src_layout, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(cb, f->preview->img, color, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    const VkPipelineLayout layout = v->pipe_layout[VP_DOWNSCALE];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipes[VP_DOWNSCALE]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &f->down_set, 0, nullptr);
    const GpuParams pc = { { f->pw, f->ph, 0, 0 }, {} };
    vkCmdPushConstants(cb, layout, VK_PC_STAGES, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (f->pw + 7) / 8, (f->ph + 7) / 8, 1);
    image_barrier(cb, f->preview->img, color, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy r = { 0, 0, 0, { color, 0, 0, 1 }, { 0, 0, 0 }, { f->pw, f->ph, 1 } };
    vkCmdCopyImageToBuffer(cb, f->preview->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->readback[slot]->buf, 1, &r);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
}

static void frames_report(const FrameSetup* f, BenchResult* out) {
    const f64 px = (f64)f->w * f->h;
    switch (f->test) {
    case GT_TRIANGLES:    gpu_frames_report(out, GR_TRI_COUNT, "Triangles", "Mtri/s", 1e6); break;
    case GT_PIXEL_FILL:   gpu_frames_report(out, px * GR_FILL_LAYERS, "Pixel fill", "Gpix/s", 1e9); break;
    case GT_TEXTURE_FILL: gpu_frames_report(out, px * GR_TEX_LAYERS * 4, "Texel rate", "Gtex/s", 1e9); break;
    case GT_DRAW_CALLS:   gpu_frames_report(out, GR_DRAWS, "Draw calls", "K draws/s", 1e3); break;
    case GT_SPRITES:      gpu_frames_report(out, GR_SPRITES, "Sprites", "Msprites/s", 1e6); break;
    case GT_BLIT:         gpu_frames_report(out, px * 4 * 2 * GR_BLIT_COPIES, "Copy bandwidth", "GB/s", 1e9); break;
    case GT_CLEAR:        gpu_frames_report(out, px * 4 * GR_CLEARS, "Clear bandwidth", "GB/s", 1e9); break;
    default:              gpu_frames_report(out, 0.0, nullptr, nullptr, 1.0); break;
    }
}

static void test_frames(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                        const CancelFlag* cancel) {
    VkScratch s = {};
    FrameSetup f = {};
    live_phase(live, "Setup", "");
    if (!frames_setup(v, &s, &f, test, p)) {
        res_fail(out, "GPU resource creation failed");
        scratch_free(v, &s);
        return;
    }
    const f64 warm = gpu_warmup_seconds(p), seconds = p->duration_ms / 1000.0;
    bool pending[GPU_FRAMES_IN_FLIGHT] = {};
    bool measuring = false, ok = true;
    u32 frame = 0, sample_frames = 0;
    gpu_frames_begin();
    live_phase(live, "Warm-up", "fps");
    const f64 t0 = timer_now();
    f64 t_prev = t0, t_meas = 0.0, t_sample = t0, t_preview = -1.0;
    while (!cancelled(cancel)) {
        const u32 slot = frame % GPU_FRAMES_IN_FLIGHT;
        if (!fence_wait(v, v->fence[slot])) { ok = false; break; }     // frame N - GPU_FRAMES_IN_FLIGHT
        if (pending[slot]) {
            buf_invalidate(v, f.readback[slot]);
            gpu_preview_publish(f.readback[slot]->map, f.pw, f.ph, f.pw * 4);
            pending[slot] = false;
        }
        const f64 now = timer_now();
        if (!measuring && now - t0 >= warm) {
            measuring = true;
            t_meas = now;
            live_phase(live, "Rendering", "fps");
        }
        if (measuring && now - t_meas >= seconds) break;
        const bool preview = t_preview < 0.0 || now - t_preview >= GPU_PREVIEW_INTERVAL;
        if (preview) t_preview = now;

        VkCommandBuffer cb = v->cmd[slot];
        cmd_begin(cb);
        record_frame(v, &f, cb, frame, (f32)(now - t0), preview, slot);
        vkEndCommandBuffer(cb);
        if (!submit(v, v->gfx_queue, cb, v->fence[slot])) { ok = false; break; }
        pending[slot] = preview;

        const f64 t = timer_now();
        if (measuring) gpu_frames_push((f32)((t - t_prev) * 1000.0));
        t_prev = t;
        ++frame;
        ++sample_frames;
        if (t - t_sample >= 0.05) {
            live_sample(live, sample_frames / (t - t_sample));
            t_sample = t;
            sample_frames = 0;
            progress(live, t - t0, warm + seconds, 0.0f, 1.0f);
        }
    }
    vkDeviceWaitIdle(v->dev);
    gpu_preview_clear();
    if (!ok) res_fail(out, "GPU error (device lost or timeout)");
    else if (!cancelled(cancel)) frames_report(&f, out);
    scratch_free(v, &s);
}

// ---- entry ---------------------------------------------------------------------------------------------

void gpu_vk_run(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                const CancelFlag* cancel) {
    VkCtx* v = (VkCtx*)state;
    switch (test) {
    case GT_FP32:
        test_kernel(v, VP_FP32, GC_ALU_GROUPS, GC_ALU_ITERS, GC_ALU_OPS_PER_DISPATCH, "FP32", "TFLOPS", 1e12, p, live, out, cancel);
        break;
    case GT_FP16:
        if (!v->fp16) { res_skip(out, "shaderFloat16 / shaderInt16 not supported"); break; }
        test_kernel(v, VP_FP16, GC_ALU_GROUPS, GC_ALU_ITERS, GC_ALU_OPS_PER_DISPATCH, "FP16", "TFLOPS", 1e12, p, live, out, cancel);
        break;
    case GT_INT32:
        test_kernel(v, VP_INT32, GC_ALU_GROUPS, GC_ALU_ITERS, GC_ALU_OPS_PER_DISPATCH, "INT32", "TIOPS", 1e12, p, live, out, cancel);
        break;
    case GT_LDS_BW:
        test_kernel(v, VP_LDS, GC_LDS_GROUPS, GC_LDS_ITERS, GC_LDS_BYTES_PER_DISPATCH, "LDS read", "GB/s", 1e9, p, live, out, cancel);
        break;
    case GT_VRAM_BW:  test_vram(v, p, live, out, cancel); break;
    case GT_TRANSFER: test_transfer(v, p, live, out, cancel); break;
    default:          test_frames(v, test, p, live, out, cancel); break;
    }
}
