// Vulkan feature tests (gpu_features.h): ray query ray tracing, VK_EXT_mesh_shader vs the vertex pipeline,
// tessellation, fragment shading rate (VRS) and async compute. Sampler feedback has no Vulkan equivalent.
// Offscreen target + preview and frame timing as in gpu_vk_tests.cpp (contract: gpu_common.h).
#include "gpu/gpu_vk_internal.h"
#include "gpu/gpu_features.h"
#include "core/results.h"
#include "core/timer.h"
#include <stdlib.h>
#include <string.h>

#include "cs_rt_terrain_spv.h"
#include "cs_rt_trace_spv.h"
#include "vs_mesh_spv.h"
#include "ps_mesh_spv.h"
#include "as_mesh_spv.h"
#include "ms_mesh_spv.h"
#include "vs_tess_spv.h"
#include "hs_tess_spv.h"
#include "ds_tess_spv.h"
#include "ps_tess_spv.h"
#include "vs_fullscreen_spv.h"
#include "ps_vrs_spv.h"

constexpr u32 FIF = GPU_FRAMES_IN_FLIGHT;
constexpr u32 TS_PER_SLOT = 8;

static bool cancelled(const CancelFlag* c) { return c->load(std::memory_order_relaxed); }

// ---- device creation hook ------------------------------------------------------------------------------

void vk_features_prepare(VkCtx* v, VkFeatureChain* fc, VkPhysicalDeviceVulkan12Features* e12, VkPhysicalDeviceFeatures* features) {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(v->phys, nullptr, &n, nullptr);
    VkExtensionProperties* props = (VkExtensionProperties*)malloc(sizeof(VkExtensionProperties) * (n ? n : 1));
    if (!props) return;
    vkEnumerateDeviceExtensionProperties(v->phys, nullptr, &n, props);
    auto has = [&](const char* name) {
        for (u32 i = 0; i < n; ++i)
            if (strcmp(props[i].extensionName, name) == 0) return true;
        return false;
    };
    const bool has_rt = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && has(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                        has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool has_mesh = has(VK_EXT_MESH_SHADER_EXTENSION_NAME), has_fsr = has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    free(props);

    // Query only the structs of extensions the device has.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    void** tail = &f12.pNext;
    if (has_rt) { *tail = &as; as.pNext = &rq; tail = &rq.pNext; }
    if (has_mesh) { *tail = &mesh; tail = &mesh.pNext; }
    if (has_fsr) { *tail = &fsr; tail = &fsr.pNext; }
    vkGetPhysicalDeviceFeatures2(v->phys, &f);

    v->ray_query = has_rt && as.accelerationStructure && rq.rayQuery && f12.bufferDeviceAddress;
    v->mesh_shader = has_mesh && mesh.meshShader && mesh.taskShader;
    v->tessellation = f.features.tessellationShader;
    v->fsr = has_fsr && fsr.pipelineFragmentShadingRate;
    v->fsr_attachment = v->fsr && fsr.attachmentFragmentShadingRate;

    // Enabled structs: only the bits the tests use.
    void** next = &fc->next;
    if (v->ray_query) {
        fc->exts[fc->ext_count++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        fc->exts[fc->ext_count++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        fc->exts[fc->ext_count++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        fc->as = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        fc->as.accelerationStructure = VK_TRUE;
        fc->rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        fc->rq.rayQuery = VK_TRUE;
        *next = &fc->as;
        fc->as.pNext = &fc->rq;
        next = &fc->rq.pNext;
        e12->bufferDeviceAddress = VK_TRUE;
    }
    if (v->mesh_shader) {
        fc->exts[fc->ext_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
        fc->mesh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        fc->mesh.meshShader = fc->mesh.taskShader = VK_TRUE;
        *next = &fc->mesh;
        next = &fc->mesh.pNext;
    }
    if (v->fsr) {
        fc->exts[fc->ext_count++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
        fc->fsr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
        fc->fsr.pipelineFragmentShadingRate = VK_TRUE;
        fc->fsr.attachmentFragmentShadingRate = v->fsr_attachment;
        *next = &fc->fsr;
        next = &fc->fsr.pNext;
    }
    if (v->tessellation) features->tessellationShader = VK_TRUE;
}

// ---- buffers with device addresses, shaders, pipelines ------------------------------------------------------

struct FBuf {
    VkBuffer buf;
    VkDeviceMemory mem;
    u8* map;                    // host-visible buffers only
    VkDeviceAddress addr;       // buffers with SHADER_DEVICE_ADDRESS usage only
};

static i32 memory_type(VkCtx* v, u32 bits, VkMemoryPropertyFlags want) {
    const VkMemoryPropertyFlags basic = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (u32 i = 0; i < v->mem_props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = v->mem_props.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & ~basic)) return (i32)i;
    }
    return -1;
}

static void fbuf_destroy(VkCtx* v, FBuf* b) {
    if (b->buf) vkDestroyBuffer(v->dev, b->buf, nullptr);
    if (b->mem) vkFreeMemory(v->dev, b->mem, nullptr);
    *b = {};
}

// host = host-visible + coherent (mapped), otherwise device-local.
static bool fbuf_create(VkCtx* v, FBuf* b, u64 size, VkBufferUsageFlags usage, bool host) {
    *b = {};
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(v->dev, &ci, nullptr, &b->buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->dev, b->buf, &req);
    const i32 type = memory_type(v, req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &flags : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (u32)type;
    bool ok = type >= 0 && vkAllocateMemory(v->dev, &ai, nullptr, &b->mem) == VK_SUCCESS &&
              vkBindBufferMemory(v->dev, b->buf, b->mem, 0) == VK_SUCCESS;
    if (ok && host) ok = vkMapMemory(v->dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void**)&b->map) == VK_SUCCESS;
    if (ok && (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) {
        VkBufferDeviceAddressInfo bi = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        bi.buffer = b->buf;
        b->addr = vkGetBufferDeviceAddress(v->dev, &bi);
    }
    if (!ok) fbuf_destroy(v, b);
    return ok;
}

// Synchronous upload through a staging buffer (graphics queue).
static bool fbuf_upload(VkCtx* v, FBuf* dst, const void* data, u64 bytes) {
    FBuf staging;
    if (!fbuf_create(v, &staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(staging.map, data, bytes);
    VkCommandBuffer cb = once_begin(v);
    const VkBufferCopy r = { 0, 0, bytes };
    vkCmdCopyBuffer(cb, staging.buf, dst->buf, 1, &r);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT);
    const bool ok = once_submit(v);
    fbuf_destroy(v, &staging);
    return ok;
}

struct Shader {
    VkShaderStageFlagBits stage;
    const u8* code;
    u32 size;
    const char* entry;
};
#define SHADER(stage, name) Shader{ stage, g_##name##_spv, (u32)sizeof(g_##name##_spv), #name }

static VkShaderModule make_module(VkCtx* v, const Shader* s) {
    u32* words = (u32*)malloc(s->size);     // the generated arrays aren't 4-byte aligned
    if (!words) return VK_NULL_HANDLE;
    memcpy(words, s->code, s->size);
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = s->size;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(v->dev, &ci, nullptr, &m);
    free(words);
    return m;
}

// Pipeline layout: one optional set layout + push constants (GpuParams) for `stages`.
static VkPipelineLayout make_layout(VkCtx* v, VkDescriptorSetLayout set, VkShaderStageFlags stages) {
    const VkPushConstantRange pcr = { stages, 0, sizeof(GpuParams) };
    VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ci.setLayoutCount = set ? 1 : 0;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &pcr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    vkCreatePipelineLayout(v->dev, &ci, nullptr, &l);
    return l;
}

static VkPipeline make_compute(VkCtx* v, VkPipelineLayout layout, Shader s) {
    VkShaderModule m = make_module(v, &s);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, s.entry };
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(v->dev, m, nullptr);
    return p;
}

struct GfxDesc {
    Shader stages[4];
    u32 stage_count;
    VkPipelineLayout layout;
    VkPrimitiveTopology topology;
    u32 patch_points;           // tessellation: control points per patch
    bool depth, cull_back;
    bool mesh;                  // task + mesh pipeline (no vertex input / input assembly)
    bool fsr_dynamic;           // per-draw fragment shading rate (dynamic state)
    bool fsr_attachment;        // may render with a shading-rate attachment
    bool depth_only;            // no color attachment, depth test ALWAYS (shadow-map style pass)
};

static VkPipeline make_graphics(VkCtx* v, const GfxDesc* g) {
    VkShaderModule mods[4] = {};
    VkPipelineShaderStageCreateInfo st[4] = {};
    bool ok = true;
    for (u32 i = 0; i < g->stage_count; ++i) {
        mods[i] = make_module(v, &g->stages[i]);
        ok &= mods[i] != VK_NULL_HANDLE;
        st[i] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, g->stages[i].stage, mods[i], g->stages[i].entry };
    }
    VkPipeline p = VK_NULL_HANDLE;
    if (ok) {
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = g->topology;
        VkPipelineTessellationStateCreateInfo ts = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
        ts.patchControlPoints = g->patch_points;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = g->cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;     // = D3D12's default; the flipped viewport keeps the winding
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = ds.depthWriteEnable = g->depth;
        ds.depthCompareOp = g->depth_only ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState att = {};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = g->depth_only ? 0 : 1;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = g->fsr_dynamic ? 3 : 2;
        dy.pDynamicStates = dyn;
        const VkFormat color = VK_RT_FORMAT;
        VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        ri.colorAttachmentCount = g->depth_only ? 0 : 1;
        ri.pColorAttachmentFormats = &color;
        if (g->depth) ri.depthAttachmentFormat = VK_DEPTH_FORMAT;

        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
        if (g->fsr_attachment) ci.flags = VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        ci.stageCount = g->stage_count;
        ci.pStages = st;
        ci.pVertexInputState = g->mesh ? nullptr : &vi;
        ci.pInputAssemblyState = g->mesh ? nullptr : &ia;
        ci.pTessellationState = g->patch_points ? &ts : nullptr;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = g->layout;
        vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    }
    for (u32 i = 0; i < g->stage_count; ++i)
        if (mods[i]) vkDestroyShaderModule(v->dev, mods[i], nullptr);
    return p;
}

// ---- offscreen target, preview and the frame loop ---------------------------------------------------------

// Everything that may still touch the target from the previous frame (render, compute write, preview read).
constexpr VkPipelineStageFlags2 PREV_STAGES = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 PREV_WRITES = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

struct Target {
    u32 w, h, pw, ph;
    bool storage;                   // written by a compute shader (GENERAL) instead of rendered to
    VkImg* rt;
    VkImg* depth;
    VkImg* preview;
    VkBuf* readback[FIF];
    VkDescriptorSet down_set;
    bool pending[FIF];
};

static bool target_create(VkCtx* v, VkScratch* s, Target* t, const BenchParams* p, bool storage, bool depth) {
    t->w = p->gpu_width < 64 ? 64 : p->gpu_width;
    t->h = p->gpu_height < 64 ? 64 : p->gpu_height;
    t->storage = storage;
    gpu_preview_size(t->w, t->h, &t->pw, &t->ph);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!(t->rt = scratch_img(v, s, t->w, t->h, VK_RT_FORMAT, usage))) return false;
    if (depth && !(t->depth = scratch_img(v, s, t->w, t->h, VK_DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        return false;
    t->preview = scratch_img(v, s, t->pw, t->ph, VK_RT_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!t->preview) return false;
    for (u32 i = 0; i < FIF; ++i)
        if (!(t->readback[i] = scratch_buf(v, s, (u64)t->pw * t->ph * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MEM_READBACK)))
            return false;
    if (!(t->down_set = desc_alloc(v, VL_DOWNSCALE))) return false;
    desc_image(v, t->down_set, 0, t->preview, true);
    desc_image(v, t->down_set, 8, t->rt, false);
    return true;
}

// Discards the target (and depth), begins dynamic rendering with clears, sets the flipped viewport.
static void begin_render(const Target* t, VkCommandBuffer cb, const void* rendering_next) {
    image_barrier(cb, t->rt->img, VK_IMAGE_ASPECT_COLOR_BIT, PREV_STAGES, PREV_WRITES, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = t->rt->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = { { 0.04f, 0.05f, 0.09f, 1.0f } };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, rendering_next };
    ri.renderArea = { { 0, 0 }, { t->w, t->h } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    if (t->depth) {
        const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        image_barrier(cb, t->depth->img, VK_IMAGE_ASPECT_DEPTH_BIT, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, tests,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        da.imageView = t->depth->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil = { 1.0f, 0 };
        ri.pDepthAttachment = &da;
    }
    vkCmdBeginRendering(cb, &ri);
    const VkViewport vp = { 0.0f, (f32)t->h, (f32)t->w, -(f32)t->h, 0.0f, 1.0f };
    const VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

// Downscale the target into the preview image and copy it to this slot's readback buffer.
static void record_preview(VkCtx* v, const Target* t, VkCommandBuffer cb, u32 slot) {
    const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    if (t->storage)
        image_barrier(cb, t->rt->img, color, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    else
        image_barrier(cb, t->rt->img, color, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_barrier(cb, t->preview->img, color, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    const VkPipelineLayout layout = v->pipe_layout[VP_DOWNSCALE];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipes[VP_DOWNSCALE]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &t->down_set, 0, nullptr);
    const GpuParams pc = { { t->pw, t->ph, 0, 0 }, {} };
    vkCmdPushConstants(cb, layout, VK_PC_STAGES, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (t->pw + 7) / 8, (t->ph + 7) / 8, 1);
    image_barrier(cb, t->preview->img, color, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy r = { 0, 0, 0, { color, 0, 0, 1 }, { 0, 0, 0 }, { t->pw, t->ph, 1 } };
    vkCmdCopyImageToBuffer(cb, t->preview->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->readback[slot]->buf, 1, &r);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
}

typedef void (*RecordFn)(VkCtx* v, void* ctx, VkCommandBuffer cb, u32 frame, u32 slot, f32 time);
typedef bool (*RetireFn)(VkCtx* v, void* ctx, u32 slot, bool measured);    // false on GPU error

struct Pass {
    const char* phase;
    f64 warmup, seconds;
    f32 p0, p1;
    RecordFn record;            // records one frame into cb (may also submit to other queues)
    RetireFn retire;            // optional: the frame last recorded in `slot` has completed on the graphics queue
    void* ctx;
};

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

static bool submit(VkCtx* v, VkQueue queue, VkCommandBuffer cb, VkFence fence) {
    vkResetFences(v->dev, 1, &fence);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    return vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS;
}

// Frame N waits for frame N - FIF, then records and submits; frame time = submit-to-submit (QPC).
// Measured frame times go to feat_pass_*. Returns false on GPU error (*error set) or cancel.
static bool run_pass(VkCtx* v, Target* t, const Pass* ps, BenchLive* live, const CancelFlag* cancel, bool* error) {
    bool used[FIF] = {}, measured[FIF] = {}, measuring = false, ok = true;
    u32 frame = 0, sample_frames = 0;
    feat_pass_begin();
    live_phase(live, "Warm-up", "fps");
    const f64 t0 = timer_now();
    f64 t_prev = t0, t_meas = 0.0, t_sample = t0, t_preview = -1.0;
    while (!cancelled(cancel)) {
        const u32 slot = frame % FIF;
        if (!fence_wait(v, v->fence[slot])) { ok = false; break; }
        if (used[slot] && ps->retire && !ps->retire(v, ps->ctx, slot, measured[slot])) { ok = false; break; }
        used[slot] = false;
        if (t->pending[slot]) {
            buf_invalidate(v, t->readback[slot]);
            gpu_preview_publish(t->readback[slot]->map, t->pw, t->ph, t->pw * 4);
            t->pending[slot] = false;
        }
        const f64 now = timer_now();
        if (!measuring && now - t0 >= ps->warmup) {
            measuring = true;
            t_meas = now;
            live_phase(live, ps->phase, "fps");
        }
        if (measuring && now - t_meas >= ps->seconds) break;
        const bool preview = t_preview < 0.0 || now - t_preview >= GPU_PREVIEW_INTERVAL;
        if (preview) t_preview = now;

        VkCommandBuffer cb = v->cmd[slot];
        cmd_begin(cb);
        ps->record(v, ps->ctx, cb, frame, slot, (f32)(now - t0));
        if (preview) record_preview(v, t, cb, slot);
        vkEndCommandBuffer(cb);
        if (!submit(v, v->gfx_queue, cb, v->fence[slot])) { ok = false; break; }
        t->pending[slot] = preview;
        used[slot] = true;
        measured[slot] = measuring;

        const f64 tn = timer_now();
        if (measuring) feat_pass_push((f32)((tn - t_prev) * 1000.0));
        t_prev = tn;
        ++frame;
        ++sample_frames;
        if (tn - t_sample >= 0.05) {
            live_sample(live, sample_frames / (tn - t_sample));
            t_sample = tn;
            sample_frames = 0;
            const f64 frac = (tn - t0) / (ps->warmup + ps->seconds);
            live_progress(live, ps->p0 + (ps->p1 - ps->p0) * (f32)(frac < 1.0 ? frac : 1.0));
        }
    }
    vkDeviceWaitIdle(v->dev);
    for (u32 i = 0; i < FIF; ++i) {
        if (used[i] && ps->retire && !ps->retire(v, ps->ctx, i, measured[i])) ok = false;
        t->pending[i] = false;
    }
    if (!ok) *error = true;
    return ok && !cancelled(cancel);
}

// Timestamp pool: TS_PER_SLOT queries per frame slot.
static VkQueryPool make_query_pool(VkCtx* v) {
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = FIF * TS_PER_SLOT;
    VkQueryPool pool = VK_NULL_HANDLE;
    vkCreateQueryPool(v->dev, &qi, nullptr, &pool);
    return pool;
}

// Reads `count` timestamps of `slot` as ms relative to the first one.
static bool read_timestamps(VkCtx* v, VkQueryPool pool, u32 slot, u32 count, f64* ms) {
    u64 ts[TS_PER_SLOT];
    if (vkGetQueryPoolResults(v->dev, pool, slot * TS_PER_SLOT, count, sizeof(ts), ts, sizeof(u64),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return false;
    for (u32 i = 0; i < count; ++i) ms[i] = (f64)((ts[i] - ts[0]) & v->ts_mask) * v->ts_period_ns * 1e-6;
    return true;
}

static void timestamp(VkCtx* v, VkQueryPool pool, VkCommandBuffer cb, u32 slot, u32 i) {
    (void)v;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, slot * TS_PER_SLOT + i);
}

// ---- ray tracing ---------------------------------------------------------------------------------------------

struct RtFn {
    PFN_vkCreateAccelerationStructureKHR create;
    PFN_vkDestroyAccelerationStructureKHR destroy;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes;
    PFN_vkCmdBuildAccelerationStructuresKHR build;
    PFN_vkGetAccelerationStructureDeviceAddressKHR address;
};

struct Accel {
    FBuf buf;
    VkAccelerationStructureKHR as;
    VkDeviceAddress addr;
};

struct Rt {
    RtFn fn;
    VkQueryPool ts;
    VkDescriptorSetLayout deform_dsl, trace_dsl;
    VkPipelineLayout deform_layout, trace_layout;
    VkDescriptorPool pool;
    VkDescriptorSet deform_set, trace_set;
    VkPipeline deform, trace;
    FBuf terrain_vb, terrain_ib, sphere_vb, sphere_ib;
    FBuf scratch, tlas_scratch;         // device addresses aligned at use
    FBuf instances[FIF];                // host-visible
    FBuf counter, counter_rb;           // rays traced in the frame; readback 8 bytes per slot (host-visible)
    Accel blas_terrain, blas_sphere, tlas;
    VkAccelerationStructureGeometryKHR terrain_geo, sphere_geo, tlas_geo;
    u64 scratch_align;
    const Target* t;
    u64 rays;
    f64 refit_ms, tlas_ms, trace_ms;
    u32 timed;
};

static void accel_destroy(VkCtx* v, Rt* r, Accel* a) {
    if (a->as) r->fn.destroy(v->dev, a->as, nullptr);
    fbuf_destroy(v, &a->buf);
    *a = {};
}

static void rt_release(VkCtx* v, Rt* r) {
    vkDeviceWaitIdle(v->dev);
    if (r->fn.destroy) {
        accel_destroy(v, r, &r->tlas);
        accel_destroy(v, r, &r->blas_sphere);
        accel_destroy(v, r, &r->blas_terrain);
    }
    FBuf* bufs[] = { &r->terrain_vb, &r->terrain_ib, &r->sphere_vb, &r->sphere_ib, &r->scratch, &r->tlas_scratch,
                     &r->counter, &r->counter_rb, &r->instances[0], &r->instances[1] };
    for (FBuf* b : bufs) fbuf_destroy(v, b);
    if (r->deform) vkDestroyPipeline(v->dev, r->deform, nullptr);
    if (r->trace) vkDestroyPipeline(v->dev, r->trace, nullptr);
    if (r->deform_layout) vkDestroyPipelineLayout(v->dev, r->deform_layout, nullptr);
    if (r->trace_layout) vkDestroyPipelineLayout(v->dev, r->trace_layout, nullptr);
    if (r->deform_dsl) vkDestroyDescriptorSetLayout(v->dev, r->deform_dsl, nullptr);
    if (r->trace_dsl) vkDestroyDescriptorSetLayout(v->dev, r->trace_dsl, nullptr);
    if (r->pool) vkDestroyDescriptorPool(v->dev, r->pool, nullptr);
    if (r->ts) vkDestroyQueryPool(v->dev, r->ts, nullptr);
}

static VkDeviceAddress aligned(VkDeviceAddress a, u64 align) { return (a + align - 1) & ~(align - 1); }

// Geometry descriptions + build info for one acceleration structure.
static VkAccelerationStructureBuildGeometryInfoKHR build_info(VkAccelerationStructureTypeKHR type, VkBuildAccelerationStructureFlagsKHR flags,
                                                             const VkAccelerationStructureGeometryKHR* geo) {
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = type;
    bi.flags = flags;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = geo;
    return bi;
}

static bool accel_create(VkCtx* v, Rt* r, Accel* a, const VkAccelerationStructureBuildGeometryInfoKHR* bi, u32 prims,
                         u64* scratch, u64* update_scratch) {
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    r->fn.sizes(v->dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, bi, &prims, &sz);
    if (!fbuf_create(v, &a->buf, sz.accelerationStructureSize,
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false))
        return false;
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = a->buf.buf;
    ci.size = sz.accelerationStructureSize;
    ci.type = bi->type;
    if (r->fn.create(v->dev, &ci, nullptr, &a->as) != VK_SUCCESS) return false;
    VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    ai.accelerationStructure = a->as;
    a->addr = r->fn.address(v->dev, &ai);
    if (sz.buildScratchSize > *scratch) *scratch = sz.buildScratchSize;
    if (update_scratch && sz.updateScratchSize > *update_scratch) *update_scratch = sz.updateScratchSize;
    return true;
}

static void accel_build(Rt* r, VkCommandBuffer cb, const Accel* a, VkAccelerationStructureBuildGeometryInfoKHR bi, u32 prims,
                        const FBuf* scratch, bool update) {
    bi.dstAccelerationStructure = a->as;
    if (update) {
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        bi.srcAccelerationStructure = a->as;
    }
    bi.scratchData.deviceAddress = aligned(scratch->addr, r->scratch_align);
    const VkAccelerationStructureBuildRangeInfoKHR range = { prims, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    r->fn.build(cb, 1, &bi, &ranges);
}

constexpr VkPipelineStageFlags2 RT_STAGES = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                            VK_PIPELINE_STAGE_2_COPY_BIT;
constexpr VkAccessFlags2 RT_WRITES = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT;
constexpr VkAccessFlags2 RT_ACCESS = RT_WRITES | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                     VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_TRANSFER_READ_BIT;

static void rt_barrier(VkCommandBuffer cb) { memory_barrier(cb, RT_STAGES, RT_WRITES, RT_STAGES, RT_ACCESS); }

static const VkBuildAccelerationStructureFlagsKHR TERRAIN_FLAGS =
    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;

static void rt_deform(VkCtx* v, Rt* r, VkCommandBuffer cb, f32 time) {
    const GpuParams pc = { { FR_GRID_VERTS, 0, 0, 0 }, { time, FR_TERRAIN_SIZE / FR_GRID, FR_TERRAIN_SIZE * 0.5f, 0.0f } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->deform);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->deform_layout, 0, 1, &r->deform_set, 0, nullptr);
    vkCmdPushConstants(cb, r->deform_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (FR_GRID_VERTS + 7) / 8, (FR_GRID_VERTS + 7) / 8, 1);
    rt_barrier(cb);
    (void)v;
}

static void write_buffer_desc(VkCtx* v, VkDescriptorSet set, u32 binding, const FBuf* b) {
    VkDescriptorBufferInfo bi = { b->buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(v->dev, 1, &w, 0, nullptr);
}

static bool rt_setup(VkCtx* v, Rt* r, f64* blas_ms, f64* sphere_ms) {
#define LOAD(field, name) r->fn.field = (decltype(r->fn.field))vkGetDeviceProcAddr(v->dev, name)
    LOAD(create, "vkCreateAccelerationStructureKHR");
    LOAD(destroy, "vkDestroyAccelerationStructureKHR");
    LOAD(sizes, "vkGetAccelerationStructureBuildSizesKHR");
    LOAD(build, "vkCmdBuildAccelerationStructuresKHR");
    LOAD(address, "vkGetAccelerationStructureDeviceAddressKHR");
#undef LOAD
    if (!r->fn.create || !r->fn.destroy || !r->fn.sizes || !r->fn.build || !r->fn.address) return false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asp };
    vkGetPhysicalDeviceProperties2(v->phys, &props);
    r->scratch_align = asp.minAccelerationStructureScratchOffsetAlignment ? asp.minAccelerationStructureScratchOffsetAlignment : 256;
    if (!(r->ts = make_query_pool(v))) return false;

    // Layouts: deform (b0 vertices, b1 counter), trace (b0 image, b1 counter, b8 TLAS, b9 / b10 terrain).
    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding deform_b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs } };
    const VkDescriptorSetLayoutBinding trace_b[5] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, cs }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs },
        { 8, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, cs }, { 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs },
        { 10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, cs },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 2;
    dli.pBindings = deform_b;
    if (vkCreateDescriptorSetLayout(v->dev, &dli, nullptr, &r->deform_dsl) != VK_SUCCESS) return false;
    dli.bindingCount = 5;
    dli.pBindings = trace_b;
    if (vkCreateDescriptorSetLayout(v->dev, &dli, nullptr, &r->trace_dsl) != VK_SUCCESS) return false;
    r->deform_layout = make_layout(v, r->deform_dsl, cs);
    r->trace_layout = make_layout(v, r->trace_dsl, cs);
    if (!r->deform_layout || !r->trace_layout) return false;
    r->deform = make_compute(v, r->deform_layout, SHADER(VK_SHADER_STAGE_COMPUTE_BIT, cs_rt_terrain));
    r->trace = make_compute(v, r->trace_layout, SHADER(VK_SHADER_STAGE_COMPUTE_BIT, cs_rt_trace));
    if (!r->deform || !r->trace) return false;
    const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
                                           { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 2;
    pi.poolSizeCount = (u32)ARRAY_COUNT(sizes);
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(v->dev, &pi, nullptr, &r->pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = r->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r->deform_dsl;
    if (vkAllocateDescriptorSets(v->dev, &ai, &r->deform_set) != VK_SUCCESS) return false;
    ai.pSetLayouts = &r->trace_dsl;
    if (vkAllocateDescriptorSets(v->dev, &ai, &r->trace_set) != VK_SUCCESS) return false;

    // Geometry.
    const VkBufferUsageFlags geo = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const u64 tvb = (u64)FR_TERRAIN_VERTS * 12, tib = (u64)FR_TERRAIN_TRIS * 12;
    const u64 svb = (u64)FR_SPHERE_VERTS * 12, sib = (u64)FR_SPHERE_TRIS * 12;
    if (!fbuf_create(v, &r->terrain_vb, tvb, geo, false) || !fbuf_create(v, &r->terrain_ib, tib, geo, false) ||
        !fbuf_create(v, &r->sphere_vb, svb, geo, false) || !fbuf_create(v, &r->sphere_ib, sib, geo, false) ||
        !fbuf_create(v, &r->counter, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) ||
        !fbuf_create(v, &r->counter_rb, FIF * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
        return false;
    for (u32 i = 0; i < FIF; ++i)
        if (!fbuf_create(v, &r->instances[i], sizeof(FeatInstance) * FR_INSTANCES,
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true))
            return false;
    u8* mesh = (u8*)malloc(tib + svb + sib);
    if (!mesh) return false;
    feat_terrain_indices((u32*)mesh);
    feat_sphere_mesh((f32*)(mesh + tib), (u32*)(mesh + tib + svb));
    const bool up = fbuf_upload(v, &r->terrain_ib, mesh, tib) && fbuf_upload(v, &r->sphere_vb, mesh + tib, svb) &&
                    fbuf_upload(v, &r->sphere_ib, mesh + tib + svb, sib);
    free(mesh);
    if (!up) return false;

    auto triangles = [](VkAccelerationStructureGeometryKHR* g, const FBuf* vb, u32 verts, const FBuf* ib) {
        *g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        g->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g->flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR* t = &g->geometry.triangles;
        t->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t->vertexData.deviceAddress = vb->addr;
        t->vertexStride = 12;
        t->maxVertex = verts - 1;
        t->indexType = VK_INDEX_TYPE_UINT32;
        t->indexData.deviceAddress = ib->addr;
    };
    triangles(&r->terrain_geo, &r->terrain_vb, FR_TERRAIN_VERTS, &r->terrain_ib);
    triangles(&r->sphere_geo, &r->sphere_vb, FR_SPHERE_VERTS, &r->sphere_ib);
    r->tlas_geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    r->tlas_geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    r->tlas_geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

    const auto fast_trace = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    const auto terrain_bi = build_info(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, TERRAIN_FLAGS, &r->terrain_geo);
    const auto sphere_bi = build_info(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, fast_trace, &r->sphere_geo);
    const auto tlas_bi = build_info(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, fast_trace, &r->tlas_geo);
    u64 scratch = 0, tlas_scratch = 0;
    if (!accel_create(v, r, &r->blas_terrain, &terrain_bi, FR_TERRAIN_TRIS, &scratch, &scratch) ||
        !accel_create(v, r, &r->blas_sphere, &sphere_bi, FR_SPHERE_TRIS, &scratch, nullptr) ||
        !accel_create(v, r, &r->tlas, &tlas_bi, FR_INSTANCES, &tlas_scratch, nullptr))
        return false;
    const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (!fbuf_create(v, &r->scratch, scratch + r->scratch_align, su, false) ||
        !fbuf_create(v, &r->tlas_scratch, tlas_scratch + r->scratch_align, su, false))
        return false;

    write_buffer_desc(v, r->deform_set, 0, &r->terrain_vb);
    write_buffer_desc(v, r->deform_set, 1, &r->counter);
    desc_image(v, r->trace_set, 0, r->t->rt, true);
    write_buffer_desc(v, r->trace_set, 1, &r->counter);
    write_buffer_desc(v, r->trace_set, 9, &r->terrain_ib);
    write_buffer_desc(v, r->trace_set, 10, &r->terrain_vb);
    VkWriteDescriptorSetAccelerationStructureKHR wa = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    wa.accelerationStructureCount = 1;
    wa.pAccelerationStructures = &r->tlas.as;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &wa };
    w.dstSet = r->trace_set;
    w.dstBinding = 8;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(v->dev, 1, &w, 0, nullptr);

    // Timed builds: sphere BLAS once, terrain BLAS FR_BLAS_BUILDS times (full builds).
    VkCommandBuffer cb = once_begin(v);
    vkCmdResetQueryPool(cb, r->ts, 0, TS_PER_SLOT);
    rt_deform(v, r, cb, 0.0f);
    timestamp(v, r->ts, cb, 0, 0);
    accel_build(r, cb, &r->blas_sphere, sphere_bi, FR_SPHERE_TRIS, &r->scratch, false);
    rt_barrier(cb);
    timestamp(v, r->ts, cb, 0, 1);
    for (u32 i = 0; i < FR_BLAS_BUILDS; ++i) {
        accel_build(r, cb, &r->blas_terrain, terrain_bi, FR_TERRAIN_TRIS, &r->scratch, false);
        rt_barrier(cb);
    }
    timestamp(v, r->ts, cb, 0, 2);
    f64 ms[3];
    if (!once_submit(v) || !read_timestamps(v, r->ts, 0, 3, ms)) return false;
    *sphere_ms = ms[1];
    *blas_ms = (ms[2] - ms[1]) / FR_BLAS_BUILDS;
    return true;
}

static void rt_record(VkCtx* v, void* ctx, VkCommandBuffer cb, u32 frame, u32 slot, f32 time) {
    Rt* r = (Rt*)ctx;
    const Target* t = r->t;
    // Previous frame (trace reads, preview reads) -> this frame's writes.
    rt_barrier(cb);
    image_barrier(cb, t->rt->img, VK_IMAGE_ASPECT_COLOR_BIT, PREV_STAGES, PREV_WRITES, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdResetQueryPool(cb, r->ts, slot * TS_PER_SLOT, TS_PER_SLOT);
    timestamp(v, r->ts, cb, slot, 0);
    rt_deform(v, r, cb, time);                      // also resets the ray counter
    timestamp(v, r->ts, cb, slot, 1);
    accel_build(r, cb, &r->blas_terrain, build_info(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, TERRAIN_FLAGS, &r->terrain_geo),
                FR_TERRAIN_TRIS, &r->scratch, true);
    rt_barrier(cb);
    timestamp(v, r->ts, cb, slot, 2);
    feat_rt_instances((FeatInstance*)r->instances[slot].map, time, r->blas_terrain.addr, r->blas_sphere.addr);
    VkAccelerationStructureGeometryKHR geo = r->tlas_geo;
    geo.geometry.instances.data.deviceAddress = r->instances[slot].addr;
    accel_build(r, cb, &r->tlas, build_info(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, &geo),
                FR_INSTANCES, &r->tlas_scratch, false);
    rt_barrier(cb);
    timestamp(v, r->ts, cb, slot, 3);
    const GpuParams pc = { { t->w, t->h, frame, 0 }, { time, (f32)t->w / t->h, 0.0f, 0.0f } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->trace);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->trace_layout, 0, 1, &r->trace_set, 0, nullptr);
    vkCmdPushConstants(cb, r->trace_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (t->w + 7) / 8, (t->h + 7) / 8, 1);
    timestamp(v, r->ts, cb, slot, 4);
    rt_barrier(cb);
    const VkBufferCopy region = { 0, (u64)slot * 8, 4 };
    vkCmdCopyBuffer(cb, r->counter.buf, r->counter_rb.buf, 1, &region);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

static bool rt_retire(VkCtx* v, void* ctx, u32 slot, bool measured) {
    Rt* r = (Rt*)ctx;
    if (!measured) return true;
    f64 ms[5];
    if (!read_timestamps(v, r->ts, slot, 5, ms)) return false;
    r->rays += ((const u32*)r->counter_rb.map)[slot * 2];
    r->refit_ms += ms[2] - ms[1];
    r->tlas_ms += ms[3] - ms[2];
    r->trace_ms += ms[4] - ms[3];
    r->timed++;
    return true;
}

static void test_raytracing(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!v->ray_query) { res_skip(out, "needs VK_KHR_acceleration_structure + VK_KHR_ray_query"); return; }
    live_phase(live, "Building BVH", "");
    VkScratch s = {};
    Target t = {};
    Rt r = {};
    r.t = &t;
    f64 blas_ms = 0.0, sphere_ms = 0.0;
    bool error = false;
    if (!target_create(v, &s, &t, p, true, false) || !rt_setup(v, &r, &blas_ms, &sphere_ms)) {
        res_fail(out, "ray tracing setup failed (out of GPU memory?)");
    } else {
        Pass ps = make_pass("Tracing", p, 0, 1, rt_record, &r);
        ps.retire = rt_retire;
        const bool ok = run_pass(v, &t, &ps, live, cancel, &error);
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
    }
    if (error) res_fail(out, "GPU error (device lost or timeout)");
    gpu_preview_clear();
    rt_release(v, &r);
    scratch_free(v, &s);
}

// ---- mesh shaders vs vertex pipeline ------------------------------------------------------------------------

struct Mesh {
    PFN_vkCmdDrawMeshTasksEXT draw_mesh;
    VkPipelineLayout classic_layout, mesh_layout;
    VkPipeline classic, mesh;
    FBuf ib;
    const Target* t;
    bool use_mesh;
};

static void mesh_record(VkCtx*, void* ctx, VkCommandBuffer cb, u32, u32, f32 time) {
    Mesh* m = (Mesh*)ctx;
    begin_render(m->t, cb, nullptr);
    const GpuParams pc = { {}, { time, (f32)m->t->w / m->t->h, 0.0f, 0.0f } };
    if (m->use_mesh) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m->mesh);
        vkCmdPushConstants(cb, m->mesh_layout, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        m->draw_mesh(cb, FM_TASK_GROUPS, 1, 1);
    } else {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m->classic);
        vkCmdPushConstants(cb, m->classic_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdBindIndexBuffer(cb, m->ib.buf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, FM_INDICES, FM_OBJECTS, 0, 0, 0);
    }
    vkCmdEndRendering(cb);
}

static void test_mesh(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!v->mesh_shader) { res_skip(out, "needs VK_EXT_mesh_shader (task + mesh shaders)"); return; }
    live_phase(live, "Setup", "");
    VkScratch s = {};
    Target t = {};
    Mesh m = {};
    m.t = &t;
    m.draw_mesh = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(v->dev, "vkCmdDrawMeshTasksEXT");
    m.classic_layout = make_layout(v, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    m.mesh_layout = make_layout(v, VK_NULL_HANDLE, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT);
    GfxDesc g = {};
    g.stages[0] = SHADER(VK_SHADER_STAGE_VERTEX_BIT, vs_mesh);
    g.stages[1] = SHADER(VK_SHADER_STAGE_FRAGMENT_BIT, ps_mesh);
    g.stage_count = 2;
    g.layout = m.classic_layout;
    g.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    g.depth = g.cull_back = true;
    m.classic = m.classic_layout ? make_graphics(v, &g) : VK_NULL_HANDLE;
    g.stages[0] = SHADER(VK_SHADER_STAGE_TASK_BIT_EXT, as_mesh);
    g.stages[1] = SHADER(VK_SHADER_STAGE_MESH_BIT_EXT, ms_mesh);
    g.stages[2] = SHADER(VK_SHADER_STAGE_FRAGMENT_BIT, ps_mesh);
    g.stage_count = 3;
    g.layout = m.mesh_layout;
    g.mesh = true;
    m.mesh = m.mesh_layout ? make_graphics(v, &g) : VK_NULL_HANDLE;
    u16* idx = (u16*)malloc(FM_INDICES * 2);
    bool ok = m.draw_mesh && m.classic && m.mesh && idx && target_create(v, &s, &t, p, false, true) &&
              fbuf_create(v, &m.ib, FM_INDICES * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    if (ok) {
        feat_mesh_indices(idx);
        ok = fbuf_upload(v, &m.ib, idx, FM_INDICES * 2);
    }
    free(idx);
    bool error = false;
    if (!ok) {
        res_fail(out, "mesh shader setup failed");
    } else {
        Pass ps = make_pass("Vertex pipeline", p, 0, 2, mesh_record, &m);
        ok = run_pass(v, &t, &ps, live, cancel, &error);
        const FeatPass classic_pass = feat_pass_end();
        m.use_mesh = true;
        ps = make_pass("Mesh shaders", p, 1, 2, mesh_record, &m);
        ok = ok && run_pass(v, &t, &ps, live, cancel, &error);
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
    if (error) res_fail(out, "GPU error (device lost or timeout)");
    vkDeviceWaitIdle(v->dev);
    gpu_preview_clear();
    fbuf_destroy(v, &m.ib);
    if (m.classic) vkDestroyPipeline(v->dev, m.classic, nullptr);
    if (m.mesh) vkDestroyPipeline(v->dev, m.mesh, nullptr);
    if (m.classic_layout) vkDestroyPipelineLayout(v->dev, m.classic_layout, nullptr);
    if (m.mesh_layout) vkDestroyPipelineLayout(v->dev, m.mesh_layout, nullptr);
    scratch_free(v, &s);
}

// ---- tessellation ------------------------------------------------------------------------------------------

struct Tess {
    VkPipelineLayout layout;
    VkPipeline pipe;
    const Target* t;
};

constexpr VkShaderStageFlags TESS_PC = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

static void tess_record(VkCtx*, void* ctx, VkCommandBuffer cb, u32, u32, f32 time) {
    Tess* ts = (Tess*)ctx;
    begin_render(ts->t, cb, nullptr);
    const GpuParams pc = { { FT_GRID, 0, 0, 0 }, { time, (f32)ts->t->w / ts->t->h, FT_FACTOR, FT_PATCH_SIZE } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ts->pipe);
    vkCmdPushConstants(cb, ts->layout, TESS_PC, 0, sizeof(pc), &pc);
    vkCmdDraw(cb, FT_GRID * FT_GRID * 4, 1, 0, 0);
    vkCmdEndRendering(cb);
}

static void test_tessellation(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!v->tessellation) { res_skip(out, "needs the tessellationShader feature"); return; }
    live_phase(live, "Setup", "");
    VkScratch s = {};
    Target t = {};
    Tess ts = {};
    ts.t = &t;
    ts.layout = make_layout(v, VK_NULL_HANDLE, TESS_PC);
    GfxDesc g = {};
    g.stages[0] = SHADER(VK_SHADER_STAGE_VERTEX_BIT, vs_tess);
    g.stages[1] = SHADER(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, hs_tess);
    g.stages[2] = SHADER(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, ds_tess);
    g.stages[3] = SHADER(VK_SHADER_STAGE_FRAGMENT_BIT, ps_tess);
    g.stage_count = 4;
    g.layout = ts.layout;
    g.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    g.patch_points = 4;
    g.depth = true;
    ts.pipe = ts.layout ? make_graphics(v, &g) : VK_NULL_HANDLE;
    bool error = false;
    if (!ts.pipe || !target_create(v, &s, &t, p, false, true)) {
        res_fail(out, "tessellation setup failed");
    } else {
        const Pass ps = make_pass("Rendering", p, 0, 1, tess_record, &ts);
        const bool ok = run_pass(v, &t, &ps, live, cancel, &error);
        const FeatPass fp = feat_pass_end();
        if (ok && fp.frames) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "Avg frame time", "ms", fp.avg_ms);
            res_add(out, "p99 frame time", "ms", fp.p99_ms);
            res_add(out, "p99.9 frame time", "ms", fp.p999_ms);
            res_add(out, "Tessellated triangles", "Mtri/s", FT_TRIS_PER_FRAME / fp.avg_ms / 1e3);
        }
    }
    if (error) res_fail(out, "GPU error (device lost or timeout)");
    vkDeviceWaitIdle(v->dev);
    gpu_preview_clear();
    if (ts.pipe) vkDestroyPipeline(v->dev, ts.pipe, nullptr);
    if (ts.layout) vkDestroyPipelineLayout(v->dev, ts.layout, nullptr);
    scratch_free(v, &s);
}

// ---- variable-rate shading (fragment shading rate) --------------------------------------------------------------

enum VrsMode : u8 { VRS_OFF, VRS_2X2, VRS_IMAGE };

struct Vrs {
    PFN_vkCmdSetFragmentShadingRateKHR set_rate;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkImg* rate_image;
    VkExtent2D texel;
    const Target* t;
    VrsMode mode;
};

static void vrs_record(VkCtx*, void* ctx, VkCommandBuffer cb, u32, u32, f32 time) {
    Vrs* r = (Vrs*)ctx;
    VkRenderingFragmentShadingRateAttachmentInfoKHR fa = { VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
    fa.imageView = r->rate_image ? r->rate_image->view : VK_NULL_HANDLE;
    fa.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    fa.shadingRateAttachmentTexelSize = r->texel;
    begin_render(r->t, cb, r->mode == VRS_IMAGE ? &fa : nullptr);
    const VkExtent2D rate = r->mode == VRS_2X2 ? VkExtent2D{ 2, 2 } : VkExtent2D{ 1, 1 };
    const VkFragmentShadingRateCombinerOpKHR comb[2] = {
        VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
        r->mode == VRS_IMAGE ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR };
    r->set_rate(cb, &rate, comb);
    const GpuParams pc = { {}, { time, (f32)r->t->w / r->t->h, 0.0f, 0.0f } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe);
    vkCmdPushConstants(cb, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cb, 3, FV_LAYERS, 0, 0);
    vkCmdEndRendering(cb);
}

// Shading-rate attachment: R8_UINT, one texel per `texel` pixels, foveated rates, left in the attachment layout.
static VkImg* make_rate_image(VkCtx* v, VkScratch* s, const Target* t, VkExtent2D texel, f64* coarse) {
    const u32 tw = (t->w + texel.width - 1) / texel.width, th = (t->h + texel.height - 1) / texel.height;
    VkImg* img = scratch_img(v, s, tw, th, VK_FORMAT_R8_UINT,
                             VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    FBuf staging;
    if (!img || !fbuf_create(v, &staging, (u64)tw * th, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return nullptr;
    *coarse = feat_vrs_rate_image(staging.map, t->w, t->h, texel.width);
    VkCommandBuffer cb = once_begin(v);
    image_barrier(cb, img->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { tw, th, 1 } };
    vkCmdCopyBufferToImage(cb, staging.buf, img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(cb, img->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
                  VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
    const bool ok = once_submit(v);
    fbuf_destroy(v, &staging);
    return ok ? img : nullptr;
}

static void test_vrs(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!v->fsr) { res_skip(out, "needs VK_KHR_fragment_shading_rate (pipeline rate)"); return; }
    live_phase(live, "Setup", "");
    VkScratch s = {};
    Target t = {};
    Vrs r = {};
    r.t = &t;
    r.set_rate = (PFN_vkCmdSetFragmentShadingRateKHR)vkGetDeviceProcAddr(v->dev, "vkCmdSetFragmentShadingRateKHR");
    // Attachment texel size: 16 x 16 when allowed (D3D12 tiles are 8-32), else the smallest supported.
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fp };
    vkGetPhysicalDeviceProperties2(v->phys, &props);
    const VkExtent2D lo = fp.minFragmentShadingRateAttachmentTexelSize, hi = fp.maxFragmentShadingRateAttachmentTexelSize;
    const u32 side = lo.width <= 16 && hi.width >= 16 && lo.height <= 16 && hi.height >= 16 ? 16 : lo.width > lo.height ? lo.width : lo.height;
    r.texel = { side, side };
    const bool image = v->fsr_attachment && side > 0 && side <= hi.width && side <= hi.height;
    r.layout = make_layout(v, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    GfxDesc g = {};
    g.stages[0] = SHADER(VK_SHADER_STAGE_VERTEX_BIT, vs_fullscreen);
    g.stages[1] = SHADER(VK_SHADER_STAGE_FRAGMENT_BIT, ps_vrs);
    g.stage_count = 2;
    g.layout = r.layout;
    g.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    g.fsr_dynamic = true;
    g.fsr_attachment = image;
    r.pipe = r.layout ? make_graphics(v, &g) : VK_NULL_HANDLE;
    bool ok = r.set_rate && r.pipe && target_create(v, &s, &t, p, false, false);
    f64 coarse = 0.0;
    if (ok && image) ok = (r.rate_image = make_rate_image(v, &s, &t, r.texel, &coarse)) != nullptr;
    bool error = false;
    if (!ok) {
        res_fail(out, "VRS setup failed");
    } else {
        const u32 passes = image ? 3 : 2;
        FeatPass fps[3] = {};
        const char* names[3] = { "Shading rate 1x1", "Shading rate 2x2", "Shading rate image" };
        for (u32 i = 0; i < passes && ok; ++i) {
            r.mode = (VrsMode)i;
            const Pass ps = make_pass(names[i], p, i, passes, vrs_record, &r);
            ok = run_pass(v, &t, &ps, live, cancel, &error);
            fps[i] = feat_pass_end();
        }
        if (ok && fps[0].frames && fps[1].frames) {
            res_begin(out, RESULT_VALUES);
            res_add(out, "1x1 frame time", "ms", fps[0].avg_ms);
            res_add(out, "2x2 frame time", "ms", fps[1].avg_ms);
            res_add(out, "2x2 speedup", "%", feat_gain_pct(fps[0].avg_ms, fps[1].avg_ms));
            if (image && fps[2].frames) {
                res_add(out, "Rate image frame time", "ms", fps[2].avg_ms);
                res_add(out, "Rate image speedup", "%", feat_gain_pct(fps[0].avg_ms, fps[2].avg_ms));
                res_add(out, "Rate image coarse pixels", "%", coarse * 100.0);
            }
        }
    }
    if (error) res_fail(out, "GPU error (device lost or timeout)");
    vkDeviceWaitIdle(v->dev);
    gpu_preview_clear();
    if (r.pipe) vkDestroyPipeline(v->dev, r.pipe, nullptr);
    if (r.layout) vkDestroyPipelineLayout(v->dev, r.layout, nullptr);
    scratch_free(v, &s);
}

// ---- async compute -----------------------------------------------------------------------------------------------

struct Async {
    VkCommandPool pool;
    VkCommandBuffer cmd[FIF];           // compute queue
    VkFence fence[FIF];
    bool submitted[FIF];
    VkQueryPool ts;
    VkDescriptorSet set;                // u0 = sink
    VkPipelineLayout depth_layout;
    VkPipeline depth_pipe;              // graphics part: depth-only full-screen layers
    f64 iters;                          // cs_fp32 iterations, balanced during the serial warm-up
    f64 gfx_ms, comp_ms;                // measured serial frames: summed GPU time of each part
    u32 timed;
    const Target* t;
    bool async;
};

// Clears color + depth (begin_render), then the depth-only layers into the cleared depth buffer.
static void async_graphics(const Async* a, VkCommandBuffer cb) {
    const Target* t = a->t;
    begin_render(t, cb, nullptr);
    vkCmdEndRendering(cb);
    const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    image_barrier(cb, t->depth->img, VK_IMAGE_ASPECT_DEPTH_BIT, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, tests, rw, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = t->depth->view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { t->w, t->h } };
    ri.layerCount = 1;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cb, &ri);
    const VkViewport vp = { 0.0f, (f32)t->h, (f32)t->w, -(f32)t->h, 0.0f, 1.0f };
    const VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a->depth_pipe);
    vkCmdDraw(cb, 3, FA_DEPTH_LAYERS, 0, 0);
    vkCmdEndRendering(cb);
}

static void async_compute(VkCtx* v, Async* a, VkCommandBuffer cb) {
    // Consecutive dispatches write the same sink buffer.
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const GpuParams pc = { { (u32)a->iters, 3, 5, 7 }, { 1.0f, 1.25f, 1.5f, 1.75f } };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipes[VP_FP32]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pipe_layout[VP_FP32], 0, 1, &a->set, 0, nullptr);
    vkCmdPushConstants(cb, v->pipe_layout[VP_FP32], VK_PC_STAGES, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, GC_ALU_GROUPS, 1, 1);
}

static void async_record(VkCtx* v, void* ctx, VkCommandBuffer cb, u32, u32 slot, f32) {
    Async* a = (Async*)ctx;
    if (!a->async) {        // serial: graphics, full barrier, compute (timestamps balance the two parts)
        vkCmdResetQueryPool(cb, a->ts, slot * TS_PER_SLOT, 3);
        timestamp(v, a->ts, cb, slot, 0);
        async_graphics(a, cb);
        memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        timestamp(v, a->ts, cb, slot, 1);
        async_compute(v, a, cb);
        timestamp(v, a->ts, cb, slot, 2);
        return;
    }
    async_graphics(a, cb);
    VkCommandBuffer cc = a->cmd[slot];
    cmd_begin(cc);
    async_compute(v, a, cc);
    vkEndCommandBuffer(cc);
    a->submitted[slot] = submit(v, v->comp_queue, cc, a->fence[slot]);
}

// Async: the frame also waits for its compute work. Serial: during warm-up, scale the compute iterations
// until compute takes as long as graphics; measured frames sum the GPU time of both parts.
static bool async_retire(VkCtx* v, void* ctx, u32 slot, bool measured) {
    Async* a = (Async*)ctx;
    if (a->async) {
        if (!a->submitted[slot]) return true;
        a->submitted[slot] = false;
        return fence_wait(v, a->fence[slot]);
    }
    f64 ms[3];
    if (!read_timestamps(v, a->ts, slot, 3, ms)) return false;
    const f64 g = ms[1], c = ms[2] - ms[1];
    if (measured) {
        a->gfx_ms += g;
        a->comp_ms += c;
        a->timed++;
    } else if (g > 0.0 && c > 0.0) {
        const f64 iters = 0.5 * (a->iters + a->iters * g / c);
        a->iters = iters < 4.0 ? 4.0 : iters > 65536.0 ? 65536.0 : iters;
    }
    return true;
}

static void test_async(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (v->comp_family == ~0u) { res_skip(out, "no compute-only queue family"); return; }
    live_phase(live, "Setup", "");
    VkScratch s = {};
    Target t = {};
    Async a = {};
    a.t = &t;
    a.iters = FA_START_ITERS;
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = v->comp_family;
    bool ok = vkCreateCommandPool(v->dev, &pi, nullptr, &a.pool) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = a.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = FIF;
        ok = vkAllocateCommandBuffers(v->dev, &ai, a.cmd) == VK_SUCCESS;
    }
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (u32 i = 0; i < FIF && ok; ++i) ok = vkCreateFence(v->dev, &fi, nullptr, &a.fence[i]) == VK_SUCCESS;
    a.depth_layout = make_layout(v, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT);
    GfxDesc g = {};
    g.stages[0] = SHADER(VK_SHADER_STAGE_VERTEX_BIT, vs_fullscreen);
    g.stage_count = 1;
    g.layout = a.depth_layout;
    g.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    g.depth = g.depth_only = true;
    a.depth_pipe = a.depth_layout ? make_graphics(v, &g) : VK_NULL_HANDLE;
    ok = ok && a.depth_pipe && (a.ts = make_query_pool(v)) != VK_NULL_HANDLE && target_create(v, &s, &t, p, false, true);
    VkBuf* sink = ok ? scratch_buf(v, &s, (u64)GC_ALU_GROUPS * 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_DEVICE) : nullptr;
    ok = sink && (a.set = desc_alloc(v, VL_BUF1)) != VK_NULL_HANDLE;
    bool error = false;
    if (!ok) {
        res_fail(out, "async compute setup failed");
    } else {
        desc_buffer(v, a.set, 0, sink);
        Pass ps = make_pass("Serial (one queue)", p, 0, 2, async_record, &a);
        ps.retire = async_retire;
        ok = run_pass(v, &t, &ps, live, cancel, &error);
        const FeatPass serial = feat_pass_end();
        a.async = true;
        ps = make_pass("Async (two queues)", p, 1, 2, async_record, &a);
        ps.retire = async_retire;
        ok = ok && run_pass(v, &t, &ps, live, cancel, &error);
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
    if (error) res_fail(out, "GPU error (device lost or timeout)");
    vkDeviceWaitIdle(v->dev);
    gpu_preview_clear();
    for (u32 i = 0; i < FIF; ++i)
        if (a.fence[i]) vkDestroyFence(v->dev, a.fence[i], nullptr);
    if (a.pool) vkDestroyCommandPool(v->dev, a.pool, nullptr);
    if (a.ts) vkDestroyQueryPool(v->dev, a.ts, nullptr);
    if (a.depth_pipe) vkDestroyPipeline(v->dev, a.depth_pipe, nullptr);
    if (a.depth_layout) vkDestroyPipelineLayout(v->dev, a.depth_layout, nullptr);
    scratch_free(v, &s);
}

// ---- entry --------------------------------------------------------------------------------------------------------

void vk_feature_test(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    switch (test) {
    case GT_RAYTRACING:       test_raytracing(v, p, live, out, cancel); break;
    case GT_MESH_SHADER:      test_mesh(v, p, live, out, cancel); break;
    case GT_TESSELLATION:     test_tessellation(v, p, live, out, cancel); break;
    case GT_VRS:              test_vrs(v, p, live, out, cancel); break;
    case GT_SAMPLER_FEEDBACK: res_skip(out, "sampler feedback has no Vulkan equivalent (D3D12 only)"); break;
    case GT_ASYNC_COMPUTE:    test_async(v, p, live, out, cancel); break;
    default: break;
    }
}
