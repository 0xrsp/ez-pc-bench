// Vulkan extra tests (gpu_ext.h): device feature hook, pipeline / dispatch / image helpers and the compute
// tests (FP64, dp4a, cooperative-matrix multiply, atomics). Memory, pipeline and texture tests live in
// gpu_vk_memory.cpp, gpu_vk_pso.cpp and gpu_vk_texture.cpp.
#include "gpu/gpu_vk_ext.h"
#include "core/results.h"
#include "core/timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_fp64_spv.h"
#include "cs_dp4a_spv.h"
#include "cs_atomic_one_spv.h"
#include "cs_atomic_unique_spv.h"
#include "cs_atomic_lds_spv.h"

static const u32 g_coopmat_f16_spv[] =
#include "coopmat_f16_spv.inc"
    ;
static const u32 g_coopmat_i8_spv[] =
#include "coopmat_i8_spv.inc"
    ;

#define SPV_BLOB(name) g_##name##_spv, (u32)sizeof(g_##name##_spv), #name

constexpr u32 MAX_TIMES = 1u << 16;
static f64 s_ms[MAX_TIMES];

static bool cancelled(const CancelFlag* c) { return c->load(std::memory_order_relaxed); }

// ---- device features ------------------------------------------------------------------------------

static bool has_device_extension(VkPhysicalDevice phys, const char* name) {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    VkExtensionProperties* props = (VkExtensionProperties*)malloc(n * sizeof(VkExtensionProperties));
    if (!props) return false;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, props);
    bool found = false;
    for (u32 i = 0; i < n && !found; ++i) found = strcmp(props[i].extensionName, name) == 0;
    free(props);
    return found;
}

void vk_ext_enable_features(VkCtx* v, VkDeviceCreateInfo* ci) {
    VkExtCaps* x = &v->ext;
    const bool has_cm = has_device_extension(v->phys, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceVulkan13Features q13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, has_cm ? &cm : nullptr };
    VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &q13 };
    VkPhysicalDeviceFeatures2 q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12 };
    vkGetPhysicalDeviceFeatures2(v->phys, &q);

    VkPhysicalDeviceVulkan13Properties p13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    VkPhysicalDeviceVulkan11Properties p11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p13 };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11 };
    vkGetPhysicalDeviceProperties2(v->phys, &props);

    x->fp64 = q.features.shaderFloat64;
    x->aniso = q.features.samplerAnisotropy;
    x->max_aniso = props.properties.limits.maxSamplerAnisotropy;
    x->bc = q.features.textureCompressionBC;
    x->int_dot = q13.shaderIntegerDotProduct;
    x->int8 = q12.shaderInt8;
    x->subgroup_size = p11.subgroupSize;
    x->subgroup_size_control = q13.subgroupSizeControl && (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
    // coopmat.comp declares the Vulkan memory model and LocalSizeId; once the memory model is on, Device-scope
    // atomics (dxc's InterlockedAdd) need its device-scope feature too (validation VUID 06265).
    x->coop_matrix = has_cm && cm.cooperativeMatrix && q12.vulkanMemoryModel && q12.vulkanMemoryModelDeviceScope &&
                     q13.maintenance4;

    // Turn the supported ones on in the requested chain, keeping whatever create_device already asked for.
    VkBaseOutStructure* last = nullptr;
    for (VkBaseOutStructure* s = (VkBaseOutStructure*)ci->pNext; s; s = s->pNext) {
        last = s;
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures* f = &((VkPhysicalDeviceFeatures2*)s)->features;
            f->shaderFloat64 |= q.features.shaderFloat64;
            f->samplerAnisotropy |= q.features.samplerAnisotropy;
            f->textureCompressionBC |= q.features.textureCompressionBC;
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            VkPhysicalDeviceVulkan12Features* f = (VkPhysicalDeviceVulkan12Features*)s;
            f->shaderInt8 |= q12.shaderInt8;
            f->shaderFloat16 |= q12.shaderFloat16;
            if (x->coop_matrix) f->vulkanMemoryModel = f->vulkanMemoryModelDeviceScope = VK_TRUE;
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            VkPhysicalDeviceVulkan13Features* f = (VkPhysicalDeviceVulkan13Features*)s;
            f->shaderIntegerDotProduct |= q13.shaderIntegerDotProduct;
            f->subgroupSizeControl |= q13.subgroupSizeControl;
            if (x->coop_matrix) f->maintenance4 = VK_TRUE;
        }
    }
    u32 n = 0;
    for (u32 i = 0; i < ci->enabledExtensionCount && n < ARRAY_COUNT(x->extensions) - 1; ++i)
        x->extensions[n++] = ci->ppEnabledExtensionNames[i];
    if (x->coop_matrix && last) {
        x->coop_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
        x->coop_features.cooperativeMatrix = VK_TRUE;
        last->pNext = (VkBaseOutStructure*)&x->coop_features;
        x->extensions[n++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    } else {
        x->coop_matrix = false;
    }
    ci->enabledExtensionCount = n;
    ci->ppEnabledExtensionNames = n ? x->extensions : nullptr;
}

// ---- pipelines -------------------------------------------------------------------------------------

VkShaderModule vkx_module(VkCtx* v, const u8* spv, u32 size) {
    u32* words = (u32*)malloc(size);    // generated byte arrays need not be 4-byte aligned
    if (!words) return VK_NULL_HANDLE;
    memcpy(words, spv, size);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = size;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(v->dev, &ci, nullptr, &m);
    free(words);
    return m;
}

bool vkx_set_layout(VkCtx* v, VkxPipe* p, const VkxBinding* b, u32 nb) {
    VkDescriptorSetLayoutBinding lb[16] = {};
    for (u32 i = 0; i < nb && i < ARRAY_COUNT(lb); ++i)
        lb[i] = { b[i].binding, b[i].type, 1, VK_SHADER_STAGE_ALL, b[i].immutable };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = nb;
    dli.pBindings = lb;
    if (vkCreateDescriptorSetLayout(v->dev, &dli, nullptr, &p->dsl) != VK_SUCCESS) return false;
    const VkPushConstantRange pcr = { VK_PC_STAGES, 0, sizeof(GpuParams) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p->dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    return vkCreatePipelineLayout(v->dev, &pli, nullptr, &p->layout) == VK_SUCCESS;
}

bool vkx_compute_pipe(VkCtx* v, VkxPipe* p, const u8* spv, u32 size, const char* entry, const VkxBinding* b, u32 nb,
                      const VkSpecializationInfo* spec, u32 subgroup_size) {
    *p = {};
    if (!vkx_set_layout(v, p, b, nb)) return false;
    const VkShaderModule cs = vkx_module(v, spv, size);
    if (!cs) return false;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };
    sg.requiredSubgroupSize = subgroup_size;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, subgroup_size && v->ext.subgroup_size_control ? &sg : nullptr,
                 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, entry, spec };
    ci.layout = p->layout;
    const VkResult r = vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipe);
    vkDestroyShaderModule(v->dev, cs, nullptr);
    return r == VK_SUCCESS;
}

void vkx_pipe_destroy(VkCtx* v, VkxPipe* p) {
    if (p->pipe) vkDestroyPipeline(v->dev, p->pipe, nullptr);
    if (p->layout) vkDestroyPipelineLayout(v->dev, p->layout, nullptr);
    if (p->dsl) vkDestroyDescriptorSetLayout(v->dev, p->dsl, nullptr);
    *p = {};
}

VkDescriptorPool vkx_pool(VkCtx* v) {
    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 16 },
    };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 16;
    pi.poolSizeCount = (u32)ARRAY_COUNT(sizes);
    pi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(v->dev, &pi, nullptr, &pool);
    return pool;
}

VkDescriptorSet vkx_set(VkCtx* v, VkDescriptorPool pool, const VkxPipe* p) {
    if (!pool || !p->dsl) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p->dsl;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(v->dev, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    return set;
}

// ---- dispatch -------------------------------------------------------------------------------------------

// Before every dispatch: earlier shader / transfer writes (also from earlier submissions) are visible.
static void compute_barrier(VkCommandBuffer cb) {
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

static void record(VkCommandBuffer cb, const VkxDispatch* d) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipe->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipe->layout, 0, 1, &d->set, 0, nullptr);
    vkCmdPushConstants(cb, d->pipe->layout, VK_PC_STAGES, 0, sizeof(GpuParams), &d->pc);
}

static bool submit_wait(VkCtx* v, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    vkResetFences(v->dev, 1, &v->fence[0]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    return vkQueueSubmit(v->gfx_queue, 1, &si, v->fence[0]) == VK_SUCCESS && fence_wait(v, v->fence[0]);
}

// `count` dispatches, each between two timestamps, a barrier in between; GPU times in ms[].
static bool run_batch(VkCtx* v, const VkxDispatch* d, u32 count, f64* ms) {
    VkCommandBuffer cb = v->cmd[0];
    if (!fence_wait(v, v->fence[0])) return false;
    cmd_begin(cb);
    vkCmdResetQueryPool(cb, v->query_pool, 0, 2 * count);
    record(cb, d);
    for (u32 i = 0; i < count; ++i) {
        compute_barrier(cb);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, v->query_pool, 2 * i);
        vkCmdDispatch(cb, d->gx, d->gy, 1);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, v->query_pool, 2 * i + 1);
    }
    if (!submit_wait(v, cb)) return false;
    u64 ts[2 * VK_MAX_BATCH];
    if (vkGetQueryPoolResults(v->dev, v->query_pool, 0, 2 * count, sizeof(u64) * 2 * count, ts, sizeof(u64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return false;
    for (u32 i = 0; i < count; ++i) ms[i] = (f64)((ts[2 * i + 1] - ts[2 * i]) & v->ts_mask) * v->ts_period_ns * 1e-6;
    return true;
}

i32 vkx_timed(VkCtx* v, const VkxDispatch* d, f64 warm, f64 seconds, f64 work, const char* phase, const char* unit,
              f64 unit_div, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1, f64* ms, u32 max) {
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
        if (measuring && (now - t_meas >= seconds || n >= max)) break;
        u32 batch = avg_ms > 0.0 ? (u32)(20.0 / avg_ms) : 1;
        batch = batch < 1 ? 1 : batch > VK_MAX_BATCH ? VK_MAX_BATCH : batch;
        if (measuring && batch > max - n) batch = max - n;
        if (!run_batch(v, d, batch, batch_ms)) return -1;
        f64 sum = 0.0;
        for (u32 i = 0; i < batch; ++i) sum += batch_ms[i];
        avg_ms = sum / batch;
        if (measuring) {
            memcpy(ms + n, batch_ms, batch * sizeof(f64));
            n += batch;
        }
        if (sum > 0.0 && work > 0.0) live_sample(live, work * batch / (sum / 1000.0) / unit_div);
        gpu_ext_progress(live, timer_now() - t0, warm + seconds, p0, p1);
    }
    return (i32)n;
}

bool vkx_once(VkCtx* v, const VkxDispatch* d) {
    VkCommandBuffer cb = once_begin(v);
    compute_barrier(cb);
    record(cb, d);
    vkCmdDispatch(cb, d->gx, d->gy, 1);
    return submit_wait(v, cb);
}

// ---- images ----------------------------------------------------------------------------------------------

static i32 device_memory_type(VkCtx* v, u32 bits) {
    i32 best = -1;
    for (u32 i = 0; i < v->mem_props.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = v->mem_props.memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return (i32)i;
        if (best < 0) best = (i32)i;
    }
    return best;
}

bool vkx_image(VkCtx* v, VkxImage* img, u32 size, u32 mips, VkFormat format) {
    *img = {};
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { size, size, 1 };
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(v->dev, &ci, nullptr, &img->img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->dev, img->img, &req);
    const i32 type = device_memory_type(v, req.memoryTypeBits);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (u32)type;
    bool ok = type >= 0 && vkAllocateMemory(v->dev, &ai, nullptr, &img->mem) == VK_SUCCESS &&
              vkBindImageMemory(v->dev, img->img, img->mem, 0) == VK_SUCCESS;
    if (ok) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
        ok = vkCreateImageView(v->dev, &vi, nullptr, &img->view) == VK_SUCCESS;
    }
    if (!ok) vkx_image_destroy(v, img);
    return ok;
}

static void mip_barrier(VkCommandBuffer cb, VkImage img, u32 mips, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                        VkImageLayout old_layout, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

bool vkx_image_upload(VkCtx* v, const VkxImage* img, u32 size, u32 mips, const VkBuf* staging, const u64* offsets) {
    VkCommandBuffer cb = once_begin(v);
    mip_barrier(cb, img->img, mips, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy regions[16] = {};
    for (u32 i = 0; i < mips && i < ARRAY_COUNT(regions); ++i) {
        const u32 s = size >> i ? size >> i : 1;
        regions[i] = { offsets[i], 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 }, { 0, 0, 0 }, { s, s, 1 } };
    }
    vkCmdCopyBufferToImage(cb, staging->buf, img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions);
    mip_barrier(cb, img->img, mips, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return submit_wait(v, cb);
}

void vkx_image_destroy(VkCtx* v, VkxImage* img) {
    if (img->view) vkDestroyImageView(v->dev, img->view, nullptr);
    if (img->img) vkDestroyImage(v->dev, img->img, nullptr);
    if (img->mem) vkFreeMemory(v->dev, img->mem, nullptr);
    *img = {};
}

// ---- compute tests -------------------------------------------------------------------------------------

static const VkxBinding BIND_U0[] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr } };

struct Kernel {
    VkxPipe pipe;
    VkBuf sink;
    VkDescriptorPool pool;
    VkDescriptorSet set;
};

static bool kernel_create(VkCtx* v, Kernel* k, const u8* spv, u32 size, const char* entry, u64 sink_bytes,
                          const VkSpecializationInfo* spec = nullptr, u32 subgroup_size = 0) {
    *k = {};
    if (!vkx_compute_pipe(v, &k->pipe, spv, size, entry, BIND_U0, 1, spec, subgroup_size)) return false;
    if (!buf_create(v, &k->sink, sink_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_DEVICE)) return false;
    k->pool = vkx_pool(v);
    k->set = vkx_set(v, k->pool, &k->pipe);
    if (!k->set) return false;
    desc_buffer(v, k->set, 0, &k->sink);
    return true;
}

static void kernel_destroy(VkCtx* v, Kernel* k) {
    vkDeviceWaitIdle(v->dev);
    if (k->pool) vkDestroyDescriptorPool(v->dev, k->pool, nullptr);
    buf_destroy(v, &k->sink);
    vkx_pipe_destroy(v, &k->pipe);
    *k = {};
}

// One ALU kernel at GC_ALU_GROUPS x 256 threads, reported like the base compute tests.
static void alu_test(VkCtx* v, const u8* spv, u32 size, const char* entry, u32 iters, f64 work, const char* phase,
                     const char* unit, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    Kernel k;
    if (!kernel_create(v, &k, spv, size, entry, (u64)GC_ALU_GROUPS * 256 * 4)) {
        res_fail(out, "pipeline / resource creation failed");
        kernel_destroy(v, &k);
        return;
    }
    const VkxDispatch d = { &k.pipe, k.set, { { iters, 0x9e3779b9u, 0x7f4a7c15u, 0x85ebca6bu }, { 1.0f, 1.1f, 1.2f, 1.3f } },
                            GC_ALU_GROUPS, 1 };
    const i32 n = vkx_timed(v, &d, gpu_warmup_seconds(p), p->duration_ms / 1000.0, work, phase, unit, 1e12, live, cancel,
                            0.0f, 1.0f, s_ms, MAX_TIMES);
    if (n < 0) res_fail(out, "GPU error (device lost or timeout)");
    else if (!cancelled(cancel)) gpu_report_compute(out, unit, 1e12, work, s_ms, (u32)n);
    kernel_destroy(v, &k);
}

static void atomics_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    struct Variant { const u8* spv; u32 size; const char* entry; u32 iters; const char* name; };
    const Variant vars[3] = {
        { SPV_BLOB(cs_atomic_one), GE_ATOM_ITERS_ONE, "One address" },
        { SPV_BLOB(cs_atomic_unique), GE_ATOM_ITERS_UNIQUE, "Unique addresses" },
        { SPV_BLOB(cs_atomic_lds), GE_ATOM_ITERS_LDS, "Shared memory (LDS)" },
    };
    f64 rate[3] = {};
    for (u32 i = 0; i < 3 && !cancelled(cancel); ++i) {
        Kernel k;
        if (!kernel_create(v, &k, vars[i].spv, vars[i].size, vars[i].entry, (u64)GE_ATOM_GROUPS * 256 * 4)) {
            res_fail(out, "pipeline / resource creation failed");
            kernel_destroy(v, &k);
            return;
        }
        const f64 work = (f64)GE_ATOM_GROUPS * 256 * vars[i].iters;
        const VkxDispatch d = { &k.pipe, k.set, { { vars[i].iters, 0, 0, 0 }, {} }, GE_ATOM_GROUPS, 1 };
        const i32 n = vkx_timed(v, &d, i == 0 ? gpu_warmup_seconds(p) : 0.05, p->duration_ms / 1000.0, work, vars[i].name,
                                "Gatomics/s", 1e9, live, cancel, i / 3.0f, (i + 1) / 3.0f, s_ms, MAX_TIMES);
        kernel_destroy(v, &k);
        if (n < 0) { res_fail(out, "GPU error (device lost or timeout)"); return; }
        rate[i] = gpu_rate(work, s_ms, (u32)n) / 1e9;
    }
    if (cancelled(cancel)) return;
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < 3; ++i) res_add(out, vars[i].name, "Gatomics/s", rate[i]);
}

// Cooperative matrix: the first supported subgroup-scope shape for FP16 x FP16 -> FP32 and INT8 x INT8 -> INT32
// (16x16x16 preferred), one subgroup per workgroup.
static bool find_shape(VkCtx* v, VkComponentTypeKHR ab, VkComponentTypeKHR c, u32* m, u32* n, u32* k) {
    auto get = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)vkGetInstanceProcAddr(
        v->instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    if (!get) return false;
    VkCooperativeMatrixPropertiesKHR props[64];
    u32 count = ARRAY_COUNT(props);
    for (u32 i = 0; i < count; ++i) props[i] = { VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR };
    if (get(v->phys, &count, props) < 0) return false;
    bool found = false;
    for (u32 i = 0; i < count; ++i) {
        const VkCooperativeMatrixPropertiesKHR* q = &props[i];
        if (q->AType != ab || q->BType != ab || q->CType != c || q->ResultType != c || q->scope != VK_SCOPE_SUBGROUP_KHR ||
            q->saturatingAccumulation)
            continue;
        if (!found || (q->MSize == 16 && q->NSize == 16 && q->KSize == 16)) {
            *m = q->MSize;
            *n = q->NSize;
            *k = q->KSize;
            found = true;
        }
    }
    return found;
}

static void matrix_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!v->ext.coop_matrix) { res_skip(out, "VK_KHR_cooperative_matrix not supported"); return; }
    struct Variant { const u32* spv; u32 size; VkComponentTypeKHR ab, c; bool supported; const char* name; const char* unit; };
    const Variant vars[2] = {
        { g_coopmat_f16_spv, (u32)sizeof(g_coopmat_f16_spv), VK_COMPONENT_TYPE_FLOAT16_KHR, VK_COMPONENT_TYPE_FLOAT32_KHR,
          v->fp16, "FP16 (FP32 accumulate)", "TFLOPS" },
        { g_coopmat_i8_spv, (u32)sizeof(g_coopmat_i8_spv), VK_COMPONENT_TYPE_SINT8_KHR, VK_COMPONENT_TYPE_SINT32_KHR,
          v->ext.int8, "INT8 (INT32 accumulate)", "TOPS" },
    };
    f64 rate[2] = {};
    bool ran[2] = {};
    const u32 sg = v->ext.subgroup_size ? v->ext.subgroup_size : 32;
    for (u32 i = 0; i < 2 && !cancelled(cancel); ++i) {
        u32 shape[4] = { sg, 16, 16, 16 };     // local size, M, N, K
        if (!vars[i].supported || !find_shape(v, vars[i].ab, vars[i].c, &shape[1], &shape[2], &shape[3])) continue;
        const VkSpecializationMapEntry map[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
        const VkSpecializationInfo spec = { 4, map, sizeof(shape), shape };
        Kernel k;
        if (!kernel_create(v, &k, (const u8*)vars[i].spv, vars[i].size, "main", 64 * KiB, &spec, sg)) {
            res_fail(out, "cooperative matrix pipeline creation failed (%s)", vars[i].name);
            kernel_destroy(v, &k);
            return;
        }
        const f64 work = (f64)GE_MATRIX_GROUPS * GE_MATRIX_ITERS * 4 * 2.0 * shape[1] * shape[2] * shape[3];
        const VkxDispatch d = { &k.pipe, k.set, { { GE_MATRIX_ITERS, 1, 0, 0 }, { 0.5f, 0.25f, 0.125f, 1.0f } },
                                GE_MATRIX_GROUPS, 1 };
        const i32 n = vkx_timed(v, &d, i == 0 ? gpu_warmup_seconds(p) : 0.05, p->duration_ms / 1000.0, work, vars[i].name,
                                vars[i].unit, 1e12, live, cancel, i / 2.0f, (i + 1) / 2.0f, s_ms, MAX_TIMES);
        kernel_destroy(v, &k);
        if (n < 0) { res_fail(out, "GPU error (device lost or timeout)"); return; }
        rate[i] = gpu_rate(work, s_ms, (u32)n) / 1e12;
        ran[i] = true;
    }
    if (cancelled(cancel)) return;
    if (!ran[0] && !ran[1]) { res_skip(out, "no subgroup FP16 or INT8 cooperative matrix shape supported"); return; }
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < 2; ++i)
        if (ran[i]) res_add(out, vars[i].name, vars[i].unit, rate[i]);
}

// ---- entry ---------------------------------------------------------------------------------------------

bool gpu_vk_run_ext(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                    const CancelFlag* cancel) {
    VkCtx* v = (VkCtx*)state;
    switch (test) {
    case GT_FP64:
        if (!v->ext.fp64) res_skip(out, "shaderFloat64 not supported");
        else alu_test(v, SPV_BLOB(cs_fp64), GE_FP64_ITERS, GE_FP64_OPS_PER_DISPATCH, "FP64", "TFLOPS", p, live, out, cancel);
        return true;
    case GT_DP4A:
        if (!v->ext.int_dot) res_skip(out, "shaderIntegerDotProduct not supported");
        else alu_test(v, SPV_BLOB(cs_dp4a), GC_ALU_ITERS, GE_DP4A_OPS_PER_DISPATCH, "INT8 dot4", "TOPS", p, live, out, cancel);
        return true;
    case GT_MATRIX: matrix_test(v, p, live, out, cancel); return true;
    case GT_ATOMICS: atomics_test(v, p, live, out, cancel); return true;
    case GT_MEM_LATENCY: case GT_MEM_SWEEP: vk_ext_memory_test(v, test, p, live, out, cancel); return true;
    case GT_PSO: vk_ext_pso_test(v, p, live, out, cancel); return true;
    case GT_TEX_FORMATS: case GT_TEX_ANISO: vk_ext_texture_test(v, test, p, live, out, cancel); return true;
    default: return false;
    }
}
