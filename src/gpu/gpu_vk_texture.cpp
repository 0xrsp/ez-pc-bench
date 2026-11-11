// Vulkan texture sampling (gpu_ext.h): bilinear sampling rate per format and the cost of anisotropic filtering.
#include "gpu/gpu_vk_ext.h"
#include "core/results.h"
#include <string.h>

#include "cs_tex_sample_spv.h"
#include "cs_tex_aniso_spv.h"

#define SPV_BLOB(name) g_##name##_spv, (u32)sizeof(g_##name##_spv), #name

constexpr u32 MAX_TIMES = 1u << 15;
static f64 s_ms[MAX_TIMES];

static const VkFormat FORMATS[GF_COUNT] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
    VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
};
static const u32 ANISO_LEVEL[GE_ANISO_MODES] = { 0, 0, 2, 4, 8, 16 };     // 0 = no anisotropy

struct TexRes {
    VkSampler samplers[GE_ANISO_MODES];
    VkxPipe pipe;
    VkBuf sink, staging;
    VkxImage img;
    VkDescriptorPool pool;
    VkDescriptorSet set;
};

static void tex_free(VkCtx* v, TexRes* r) {
    vkDeviceWaitIdle(v->dev);
    if (r->pool) vkDestroyDescriptorPool(v->dev, r->pool, nullptr);
    vkx_image_destroy(v, &r->img);
    buf_destroy(v, &r->staging);
    buf_destroy(v, &r->sink);
    vkx_pipe_destroy(v, &r->pipe);
    for (VkSampler s : r->samplers)
        if (s) vkDestroySampler(v->dev, s, nullptr);
    *r = {};
}

// Samplers s0..s5 (bindings 16..21), the pipeline, sink buffer, staging buffer and descriptor set.
static bool tex_setup(VkCtx* v, TexRes* r, const u8* spv, u32 size, const char* entry, u64 staging_bytes) {
    for (u32 i = 0; i < GE_ANISO_MODES; ++i) {
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = i == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod = VK_LOD_CLAMP_NONE;
        const f32 level = (f32)ANISO_LEVEL[i];
        si.anisotropyEnable = level > 0.0f && v->ext.aniso;
        si.maxAnisotropy = level < v->ext.max_aniso ? level : v->ext.max_aniso;
        if (!si.anisotropyEnable) si.maxAnisotropy = 1.0f;
        if (vkCreateSampler(v->dev, &si, nullptr, &r->samplers[i]) != VK_SUCCESS) return false;
    }
    VkxBinding b[2 + GE_ANISO_MODES] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr },
        { 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr },
    };
    for (u32 i = 0; i < GE_ANISO_MODES; ++i) b[2 + i] = { 16 + i, VK_DESCRIPTOR_TYPE_SAMPLER, &r->samplers[i] };
    if (!vkx_compute_pipe(v, &r->pipe, spv, size, entry, b, ARRAY_COUNT(b))) return false;
    if (!buf_create(v, &r->sink, 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_DEVICE)) return false;
    if (!buf_create(v, &r->staging, staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MEM_UPLOAD)) return false;
    r->pool = vkx_pool(v);
    r->set = vkx_set(v, r->pool, &r->pipe);
    if (!r->set) return false;
    desc_buffer(v, r->set, 0, &r->sink);
    return true;
}

static bool format_usable(VkCtx* v, GeTexFormat f) {
    if (f >= GF_BC1 && !v->ext.bc) return false;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(v->phys, FORMATS[f], &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

static void formats_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    TexRes r = {};
    const u64 max_bytes = (u64)GE_TEX_SIZE * GE_TEX_SIZE * 16;
    if (!tex_setup(v, &r, SPV_BLOB(cs_tex_sample), max_bytes)) {
        res_fail(out, "pipeline / resource creation failed");
        tex_free(v, &r);
        return;
    }
    f64 seconds = p->duration_ms / 1000.0 / 8.0;
    seconds = seconds < 0.2 ? 0.2 : seconds;
    const f64 work = (f64)GE_TEX_GRID_W * GE_TEX_GRID_H * GE_TEX_SAMPLES;
    f64 rate[GF_COUNT] = {};
    bool ran[GF_COUNT] = {}, gpu_ok = true;
    for (u32 f = 0; f < GF_COUNT && gpu_ok && !cancel->load(std::memory_order_relaxed); ++f) {
        if (!format_usable(v, (GeTexFormat)f)) continue;
        vkx_image_destroy(v, &r.img);
        gpu_ext_tex_fill((GeTexFormat)f, r.staging.map, GE_TEX_SIZE, f + 1);
        const u64 offset = 0;
        if (!vkx_image(v, &r.img, GE_TEX_SIZE, 1, FORMATS[f]) || !vkx_image_upload(v, &r.img, GE_TEX_SIZE, 1, &r.staging, &offset)) {
            gpu_ok = false;
            break;
        }
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, r.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = r.set;
        w.dstBinding = 8;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(v->dev, 1, &w, 0, nullptr);
        const VkxDispatch d = { &r.pipe, r.set, { { GE_TEX_SAMPLES, 0, 0, 0 }, { 1.0f / GE_TEX_SIZE, 0, 0, 0 } },
                                GE_TEX_GRID_W / 8, GE_TEX_GRID_H / 8 };
        const i32 n = vkx_timed(v, &d, f == 0 ? gpu_warmup_seconds(p) : 0.05, seconds, work, GE_TEX_FORMAT_NAMES[f], "Gtex/s",
                                1e9, live, cancel, (f32)f / (f32)GF_COUNT, (f32)(f + 1) / (f32)GF_COUNT, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        rate[f] = gpu_rate(work, s_ms, (u32)n) / 1e9;
        ran[f] = true;
    }
    tex_free(v, &r);
    if (!gpu_ok) { res_fail(out, "GPU error or texture creation failed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_VALUES);
    for (u32 f = 0; f < GF_COUNT; ++f)
        if (ran[f]) res_add(out, GE_TEX_FORMAT_NAMES[f], "Gtex/s", rate[f]);
}

static void aniso_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    u32 mips = 0;
    u64 offsets[16], bytes = 0;
    for (u32 s = GE_TEX_SIZE; s; s >>= 1) {
        offsets[mips++] = bytes;
        bytes += (u64)s * s * 4;
    }
    TexRes r = {};
    bool ok = tex_setup(v, &r, SPV_BLOB(cs_tex_aniso), bytes);
    if (ok) {
        for (u32 i = 0; i < mips; ++i) gpu_ext_tex_fill(GF_RGBA8, r.staging.map + offsets[i], GE_TEX_SIZE >> i, 100 + i);
        ok = vkx_image(v, &r.img, GE_TEX_SIZE, mips, VK_FORMAT_R8G8B8A8_UNORM) &&
             vkx_image_upload(v, &r.img, GE_TEX_SIZE, mips, &r.staging, offsets);
    }
    if (!ok) {
        res_fail(out, "pipeline / texture creation failed");
        tex_free(v, &r);
        return;
    }
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, r.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = r.set;
    w.dstBinding = 8;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(v->dev, 1, &w, 0, nullptr);

    f64 seconds = p->duration_ms / 1000.0 / GE_ANISO_MODES;
    seconds = seconds < 0.2 ? 0.2 : seconds;
    const f64 work = (f64)GE_TEX_GRID_W * GE_TEX_GRID_H * GE_ANISO_SAMPLES;
    const f32 inv = 1.0f / GE_TEX_SIZE;
    f64 rate[GE_ANISO_MODES] = {};
    bool ran[GE_ANISO_MODES] = {}, gpu_ok = true;
    for (u32 m = 0; m < GE_ANISO_MODES && !cancel->load(std::memory_order_relaxed); ++m) {
        if (ANISO_LEVEL[m] && (!v->ext.aniso || (f32)ANISO_LEVEL[m] > v->ext.max_aniso)) continue;
        const VkxDispatch d = { &r.pipe, r.set, { { GE_ANISO_SAMPLES, m, 0, 0 }, { inv, inv * GE_ANISO_RATIO, inv, 0 } },
                                GE_TEX_GRID_W / 8, GE_TEX_GRID_H / 8 };
        const i32 n = vkx_timed(v, &d, m == 0 ? gpu_warmup_seconds(p) : 0.05, seconds, work, GE_ANISO_NAMES[m], "Gtex/s", 1e9,
                                live, cancel, (f32)m / GE_ANISO_MODES, (f32)(m + 1) / GE_ANISO_MODES, s_ms, MAX_TIMES);
        if (n < 0) { gpu_ok = false; break; }
        rate[m] = gpu_rate(work, s_ms, (u32)n) / 1e9;
        ran[m] = true;
    }
    tex_free(v, &r);
    if (!gpu_ok) { res_fail(out, "GPU error (device lost or timeout)"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_VALUES);
    for (u32 m = 0; m < GE_ANISO_MODES; ++m)
        if (ran[m]) res_add(out, GE_ANISO_NAMES[m], "Gtex/s", rate[m]);
}

void vk_ext_texture_test(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                         const CancelFlag* cancel) {
    if (test == GT_TEX_FORMATS) formats_test(v, p, live, out, cancel);
    else aniso_test(v, p, live, out, cancel);
}
