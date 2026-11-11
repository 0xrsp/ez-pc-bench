// Vulkan backend: instance / device selection by LUID, layouts, pipelines and resource helpers.
// The tests themselves live in gpu_vk_tests.cpp. Contract: gpu_common.h.
#include "gpu/gpu_vk_internal.h"
#include "core/results.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_fp32_spv.h"
#include "cs_fp16_spv.h"
#include "cs_int32_spv.h"
#include "cs_bw_read_spv.h"
#include "cs_bw_write_spv.h"
#include "cs_bw_copy_spv.h"
#include "cs_lds_spv.h"
#include "cs_downscale_spv.h"
#include "vs_triangles_spv.h"
#include "vs_drawcall_spv.h"
#include "ps_color_spv.h"
#include "vs_fullscreen_spv.h"
#include "ps_fill_spv.h"
#include "ps_texfill_spv.h"
#include "vs_scene_spv.h"
#include "ps_scene_spv.h"
#include "vs_sprite_spv.h"
#include "ps_sprite_spv.h"

static VkCtx s_vk;

// ---- pipeline table ------------------------------------------------------------------------------

enum PipeKind : u8 { PK_COMPUTE, PK_OPAQUE, PK_DEPTH, PK_BLEND };

struct ShaderBlob { const u8* code; u32 size; const char* entry; };
#define SPV(name) { g_##name##_spv, (u32)sizeof(g_##name##_spv), #name }
#define NO_SPV { nullptr, 0, nullptr }

struct PipeDesc {
    PipeKind kind;
    VkLayoutId layout;
    ShaderBlob cs_or_vs;
    ShaderBlob ps;
};

static const PipeDesc PIPES[VP_COUNT] = {
    { PK_COMPUTE, VL_BUF1,      SPV(cs_fp32),       NO_SPV },
    { PK_COMPUTE, VL_BUF1,      SPV(cs_fp16),       NO_SPV },
    { PK_COMPUTE, VL_BUF1,      SPV(cs_int32),      NO_SPV },
    { PK_COMPUTE, VL_BUF2,      SPV(cs_bw_read),    NO_SPV },
    { PK_COMPUTE, VL_BUF2,      SPV(cs_bw_write),   NO_SPV },
    { PK_COMPUTE, VL_BUF2,      SPV(cs_bw_copy),    NO_SPV },
    { PK_COMPUTE, VL_BUF1,      SPV(cs_lds),        NO_SPV },
    { PK_COMPUTE, VL_DOWNSCALE, SPV(cs_downscale),  NO_SPV },
    { PK_OPAQUE,  VL_NONE,      SPV(vs_triangles),  SPV(ps_color) },
    { PK_OPAQUE,  VL_NONE,      SPV(vs_drawcall),   SPV(ps_color) },
    { PK_OPAQUE,  VL_NONE,      SPV(vs_fullscreen), SPV(ps_fill) },
    { PK_OPAQUE,  VL_TEX,       SPV(vs_fullscreen), SPV(ps_texfill) },
    { PK_DEPTH,   VL_NONE,      SPV(vs_scene),      SPV(ps_scene) },
    { PK_BLEND,   VL_TEX,       SPV(vs_sprite),     SPV(ps_sprite) },
};

// ---- debug messenger (debug builds) ---------------------------------------------------------------------

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* tag = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning";
    char line[2048];
    snprintf(line, sizeof(line), "[vulkan %s] %s\n", tag, data->pMessage ? data->pMessage : "");
    fputs(line, stderr);
    OutputDebugStringA(line);
    return VK_FALSE;
}

#ifndef NDEBUG
static bool has_layer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    VkLayerProperties props[64];
    if (n > ARRAY_COUNT(props)) n = ARRAY_COUNT(props);
    vkEnumerateInstanceLayerProperties(&n, props);
    for (u32 i = 0; i < n; ++i)
        if (strcmp(props[i].layerName, name) == 0) return true;
    return false;
}
#endif

// ---- device -----------------------------------------------------------------------------------------------

static bool fail(char* error, u32 error_size, const char* what, VkResult r) {
    snprintf(error, error_size, "%s failed (VkResult %d)", what, (int)r);
    return false;
}

static bool create_instance(VkCtx* v, char* error, u32 error_size) {
    u32 api = 0;
    if (vkEnumerateInstanceVersion(&api) != VK_SUCCESS || api < VK_API_VERSION_1_3) {
        snprintf(error, error_size, "Vulkan 1.3 loader required");
        return false;
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ez-pc-bench";
    app.pEngineName = "ez-pc-bench";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    bool debug = false;
#ifndef NDEBUG
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (has_layer(layer)) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = &ext;
        debug = true;
    } else {
        fputs("[vulkan] validation layer not available\n", stderr);
    }
#endif
    VkResult r = vkCreateInstance(&ci, nullptr, &v->instance);
    if (r != VK_SUCCESS) return fail(error, error_size, "vkCreateInstance", r);

    if (debug) {
        auto create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(v->instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = debug_callback;
        if (create_messenger) create_messenger(v->instance, &mi, nullptr, &v->messenger);
    }
    return true;
}

static bool pick_device(VkCtx* v, const GpuInfo* gi, char* error, u32 error_size) {
    u32 n = 0;
    vkEnumeratePhysicalDevices(v->instance, &n, nullptr);
    VkPhysicalDevice devs[16];
    if (n > ARRAY_COUNT(devs)) n = ARRAY_COUNT(devs);
    vkEnumeratePhysicalDevices(v->instance, &n, devs);

    u8 want[VK_LUID_SIZE];
    memcpy(want, &gi->luid_low, 4);
    memcpy(want + 4, &gi->luid_high, 4);
    for (u32 i = 0; i < n; ++i) {
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
        vkGetPhysicalDeviceProperties2(devs[i], &props);
        if (!id.deviceLUIDValid || memcmp(id.deviceLUID, want, VK_LUID_SIZE) != 0) continue;
        if (props.properties.apiVersion < VK_API_VERSION_1_3) {
            snprintf(error, error_size, "%s: Vulkan 1.3 not supported", props.properties.deviceName);
            return false;
        }
        v->phys = devs[i];
        v->limits = props.properties.limits;
        v->ts_period_ns = props.properties.limits.timestampPeriod;
        return true;
    }
    snprintf(error, error_size, "no Vulkan device matches adapter '%s'", gi->name);
    return false;
}

static bool create_device(VkCtx* v, char* error, u32 error_size) {
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    vkGetPhysicalDeviceFeatures2(v->phys, &f);
    if (!f13.dynamicRendering || !f13.synchronization2) {
        snprintf(error, error_size, "dynamic rendering / synchronization2 not supported");
        return false;
    }
    v->fp16 = f12.shaderFloat16 && f.features.shaderInt16;

    // Queues: graphics+compute with timestamps; a dedicated transfer (DMA) family if there is one.
    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, nullptr);
    VkQueueFamilyProperties qf[16];
    if (nq > ARRAY_COUNT(qf)) nq = ARRAY_COUNT(qf);
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qf);
    const VkQueueFlags gc = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    v->gfx_family = v->xfer_family = ~0u;
    for (u32 i = 0; i < nq; ++i)
        if ((qf[i].queueFlags & gc) == gc && qf[i].timestampValidBits && v->gfx_family == ~0u) v->gfx_family = i;
    if (v->gfx_family == ~0u) {
        snprintf(error, error_size, "no graphics+compute queue with timestamps");
        return false;
    }
    for (u32 i = 0; i < nq; ++i)
        if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qf[i].queueFlags & gc) && v->xfer_family == ~0u) v->xfer_family = i;
    if (v->xfer_family == ~0u) v->xfer_family = v->gfx_family;
    const u32 bits = qf[v->gfx_family].timestampValidBits;
    v->ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;

    const f32 prio = 1.0f;
    VkDeviceQueueCreateInfo qci[2] = {};
    for (u32 i = 0; i < 2; ++i) {
        qci[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci[i].queueFamilyIndex = i == 0 ? v->gfx_family : v->xfer_family;
        qci[i].queueCount = 1;
        qci[i].pQueuePriorities = &prio;
    }

    VkPhysicalDeviceVulkan13Features e13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13 };
    e12.shaderFloat16 = v->fp16;
    VkPhysicalDeviceFeatures2 ef = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12 };
    ef.features.shaderInt16 = v->fp16;

    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef };
    ci.queueCreateInfoCount = v->xfer_family != v->gfx_family ? 2 : 1;
    ci.pQueueCreateInfos = qci;
    VkResult r = vkCreateDevice(v->phys, &ci, nullptr, &v->dev);
    if (r != VK_SUCCESS) return fail(error, error_size, "vkCreateDevice", r);
    vkGetDeviceQueue(v->dev, v->gfx_family, 0, &v->gfx_queue);
    vkGetDeviceQueue(v->dev, v->xfer_family, 0, &v->xfer_queue);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mem_props);
    v->cmd_push_constants = (PFN_vkCmdPushConstants)vkGetDeviceProcAddr(v->dev, "vkCmdPushConstants");
    v->cmd_draw = (PFN_vkCmdDraw)vkGetDeviceProcAddr(v->dev, "vkCmdDraw");
    if (!v->cmd_push_constants || !v->cmd_draw) {
        snprintf(error, error_size, "vkGetDeviceProcAddr failed");
        return false;
    }
    return true;
}

// ---- layouts / pipelines -------------------------------------------------------------------------------

static bool create_layouts(VkCtx* v) {
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(v->dev, &si, nullptr, &v->sampler) != VK_SUCCESS) return false;

    const VkShaderStageFlags all = VK_SHADER_STAGE_ALL;
    const VkDescriptorSetLayoutBinding sampler_binding = { 16, VK_DESCRIPTOR_TYPE_SAMPLER, 1, all, &v->sampler };
    VkDescriptorSetLayoutBinding b[VL_COUNT][5] = {};
    u32 nb[VL_COUNT] = {};
    b[VL_BUF1][0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all };
    nb[VL_BUF1] = 1;
    b[VL_BUF2][0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all };
    b[VL_BUF2][1] = { 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, all };
    nb[VL_BUF2] = 2;
    b[VL_DOWNSCALE][0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, all };
    b[VL_DOWNSCALE][1] = { 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, all };
    b[VL_DOWNSCALE][2] = sampler_binding;
    nb[VL_DOWNSCALE] = 3;
    for (u32 i = 0; i < 4; ++i) b[VL_TEX][i] = { 8 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, all };
    b[VL_TEX][4] = sampler_binding;
    nb[VL_TEX] = 5;

    const VkPushConstantRange pcr = { VK_PC_STAGES, 0, sizeof(GpuParams) };
    for (u32 l = 0; l < VL_COUNT; ++l) {
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        if (nb[l]) {
            VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dli.bindingCount = nb[l];
            dli.pBindings = b[l];
            if (vkCreateDescriptorSetLayout(v->dev, &dli, nullptr, &v->set_layouts[l]) != VK_SUCCESS) return false;
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &v->set_layouts[l];
        }
        if (vkCreatePipelineLayout(v->dev, &pli, nullptr, &v->layouts[l]) != VK_SUCCESS) return false;
    }

    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 8 },
    };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 8;
    pi.poolSizeCount = (u32)ARRAY_COUNT(sizes);
    pi.pPoolSizes = sizes;
    return vkCreateDescriptorPool(v->dev, &pi, nullptr, &v->desc_pool) == VK_SUCCESS;
}

// The generated byte arrays are not guaranteed to be 4-byte aligned: copy before creating the module.
static VkShaderModule make_module(VkCtx* v, const ShaderBlob* s) {
    u32* words = (u32*)malloc(s->size);
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

static VkPipeline make_compute(VkCtx* v, const PipeDesc* d) {
    VkShaderModule cs = make_module(v, &d->cs_or_vs);
    if (!cs) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, d->cs_or_vs.entry };
    ci.layout = v->layouts[d->layout];
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(v->dev, cs, nullptr);
    return p;
}

static VkPipeline make_graphics(VkCtx* v, const PipeDesc* d) {
    VkShaderModule vs = make_module(v, &d->cs_or_vs), ps = make_module(v, &d->ps);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && ps) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, d->cs_or_vs.entry },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, d->ps.entry },
        };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = ds.depthWriteEnable = d->kind == PK_DEPTH;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState att = {};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (d->kind == PK_BLEND) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = (u32)ARRAY_COUNT(dyn);
        dy.pDynamicStates = dyn;
        const VkFormat color = VK_RT_FORMAT;
        VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &color;
        if (d->kind == PK_DEPTH) ri.depthAttachmentFormat = VK_DEPTH_FORMAT;

        VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = v->layouts[d->layout];
        vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    }
    if (vs) vkDestroyShaderModule(v->dev, vs, nullptr);
    if (ps) vkDestroyShaderModule(v->dev, ps, nullptr);
    return p;
}

static bool create_pipelines(VkCtx* v, char* error, u32 error_size) {
    for (u32 i = 0; i < VP_COUNT; ++i) {
        if (i == VP_FP16 && !v->fp16) continue;     // needs Float16 + Int16 capabilities
        const PipeDesc* d = &PIPES[i];
        v->pipe_layout[i] = v->layouts[d->layout];
        v->pipes[i] = d->kind == PK_COMPUTE ? make_compute(v, d) : make_graphics(v, d);
        if (!v->pipes[i]) {
            snprintf(error, error_size, "pipeline creation failed (%s)", d->ps.entry ? d->ps.entry : d->cs_or_vs.entry);
            return false;
        }
    }
    return true;
}

static bool create_commands(VkCtx* v) {
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = v->gfx_family;
    if (vkCreateCommandPool(v->dev, &pi, nullptr, &v->cmd_pool) != VK_SUCCESS) return false;
    pi.queueFamilyIndex = v->xfer_family;
    if (vkCreateCommandPool(v->dev, &pi, nullptr, &v->xfer_pool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = v->cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = GPU_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(v->dev, &ai, v->cmd) != VK_SUCCESS) return false;
    ai.commandPool = v->xfer_pool;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(v->dev, &ai, &v->xfer_cmd) != VK_SUCCESS) return false;

    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i)
        if (vkCreateFence(v->dev, &fi, nullptr, &v->fence[i]) != VK_SUCCESS) return false;
    if (vkCreateFence(v->dev, &fi, nullptr, &v->xfer_fence) != VK_SUCCESS) return false;

    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * VK_MAX_BATCH;
    return vkCreateQueryPool(v->dev, &qi, nullptr, &v->query_pool) == VK_SUCCESS;
}

// ---- suite entry points ------------------------------------------------------------------------------------

void gpu_vk_shutdown(void* state) {
    VkCtx* v = (VkCtx*)state;
    if (!v) return;
    if (v->dev) {
        vkDeviceWaitIdle(v->dev);
        if (v->query_pool) vkDestroyQueryPool(v->dev, v->query_pool, nullptr);
        for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i)
            if (v->fence[i]) vkDestroyFence(v->dev, v->fence[i], nullptr);
        if (v->xfer_fence) vkDestroyFence(v->dev, v->xfer_fence, nullptr);
        if (v->cmd_pool) vkDestroyCommandPool(v->dev, v->cmd_pool, nullptr);
        if (v->xfer_pool) vkDestroyCommandPool(v->dev, v->xfer_pool, nullptr);
        for (u32 i = 0; i < VP_COUNT; ++i)
            if (v->pipes[i]) vkDestroyPipeline(v->dev, v->pipes[i], nullptr);
        for (u32 i = 0; i < VL_COUNT; ++i) {
            if (v->layouts[i]) vkDestroyPipelineLayout(v->dev, v->layouts[i], nullptr);
            if (v->set_layouts[i]) vkDestroyDescriptorSetLayout(v->dev, v->set_layouts[i], nullptr);
        }
        if (v->desc_pool) vkDestroyDescriptorPool(v->dev, v->desc_pool, nullptr);
        if (v->sampler) vkDestroySampler(v->dev, v->sampler, nullptr);
        vkDestroyDevice(v->dev, nullptr);
    }
    if (v->messenger) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(v->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(v->instance, v->messenger, nullptr);
    }
    if (v->instance) vkDestroyInstance(v->instance, nullptr);
    *v = {};
}

bool gpu_vk_init(const BenchParams* p, void** state, char* error, u32 error_size) {
    *state = nullptr;
    const GpuInfo* gi = gpu_selected(p);
    if (!gi) {
        snprintf(error, error_size, "no GPU adapter found");
        return false;
    }
    VkCtx* v = &s_vk;
    *v = {};
    bool ok = create_instance(v, error, error_size) && pick_device(v, gi, error, error_size) &&
              create_device(v, error, error_size);
    if (ok && !create_layouts(v)) ok = fail(error, error_size, "descriptor / pipeline layout creation", VK_ERROR_UNKNOWN);
    if (ok) ok = create_pipelines(v, error, error_size);
    if (ok && !create_commands(v)) ok = fail(error, error_size, "command pool / fence / query pool creation", VK_ERROR_UNKNOWN);
    if (!ok) {
        gpu_vk_shutdown(v);
        return false;
    }
    *state = v;
    return true;
}

// ---- resource helpers ----------------------------------------------------------------------------------

// Picks the best memory type for `kind` among `bits`; returns -1 when none fits.
static i32 find_memory(VkCtx* v, u32 bits, MemKind kind) {
    const VkMemoryPropertyFlags basic = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    i32 best = -1, best_score = -1;
    for (u32 i = 0; i < v->mem_props.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = v->mem_props.memoryTypes[i].propertyFlags;
        if (f & ~basic) continue;   // lazily allocated, protected, AMD device-coherent, ...
        const bool dev = f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host = f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const bool coh = f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, cached = f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        i32 score = -1;
        switch (kind) {
        case MEM_DEVICE:   if (dev) score = host ? 1 : 2; break;
        case MEM_UPLOAD:   if (host && coh) score = (dev ? 0 : 4) + (cached ? 0 : 2); break;  // system RAM, write-combined
        case MEM_READBACK: if (host) score = (cached ? 4 : 0) + (coh ? 2 : 0) + (dev ? 0 : 1); break;
        }
        if (score > best_score) { best = (i32)i; best_score = score; }
    }
    return best;
}

static bool alloc_memory(VkCtx* v, const VkMemoryRequirements* req, MemKind kind, VkDeviceMemory* mem, bool* coherent) {
    const i32 type = find_memory(v, req->memoryTypeBits, kind);
    if (type < 0) return false;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req->size;
    ai.memoryTypeIndex = (u32)type;
    if (coherent) *coherent = v->mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    return vkAllocateMemory(v->dev, &ai, nullptr, mem) == VK_SUCCESS;
}

bool buf_create(VkCtx* v, VkBuf* b, u64 size, VkBufferUsageFlags usage, MemKind kind) {
    *b = {};
    b->size = size;
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(v->dev, &ci, nullptr, &b->buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->dev, b->buf, &req);
    bool ok = alloc_memory(v, &req, kind, &b->mem, &b->coherent) && vkBindBufferMemory(v->dev, b->buf, b->mem, 0) == VK_SUCCESS;
    if (ok && kind != MEM_DEVICE) ok = vkMapMemory(v->dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void**)&b->map) == VK_SUCCESS;
    if (!ok) buf_destroy(v, b);
    return ok;
}

void buf_destroy(VkCtx* v, VkBuf* b) {
    if (b->buf) vkDestroyBuffer(v->dev, b->buf, nullptr);
    if (b->mem) vkFreeMemory(v->dev, b->mem, nullptr);     // implicitly unmaps
    *b = {};
}

void buf_invalidate(VkCtx* v, const VkBuf* b) {
    if (b->coherent) return;
    VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    r.memory = b->mem;
    r.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(v->dev, 1, &r);
}

static void img_destroy(VkCtx* v, VkImg* i) {
    if (i->view) vkDestroyImageView(v->dev, i->view, nullptr);
    if (i->img) vkDestroyImage(v->dev, i->img, nullptr);
    if (i->mem) vkFreeMemory(v->dev, i->mem, nullptr);
    *i = {};
}

static bool img_create(VkCtx* v, VkImg* i, u32 w, u32 h, VkFormat format, VkImageUsageFlags usage) {
    *i = {};
    i->w = w;
    i->h = h;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(v->dev, &ci, nullptr, &i->img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->dev, i->img, &req);
    bool ok = alloc_memory(v, &req, MEM_DEVICE, &i->mem, nullptr) && vkBindImageMemory(v->dev, i->img, i->mem, 0) == VK_SUCCESS;
    if (ok) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = i->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = { format == VK_DEPTH_FORMAT ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        ok = vkCreateImageView(v->dev, &vi, nullptr, &i->view) == VK_SUCCESS;
    }
    if (!ok) img_destroy(v, i);
    return ok;
}

VkBuf* scratch_buf(VkCtx* v, VkScratch* s, u64 size, VkBufferUsageFlags usage, MemKind kind) {
    if (s->nbuf == ARRAY_COUNT(s->bufs)) return nullptr;
    VkBuf* b = &s->bufs[s->nbuf];
    if (!buf_create(v, b, size, usage, kind)) return nullptr;
    s->nbuf++;
    return b;
}

VkImg* scratch_img(VkCtx* v, VkScratch* s, u32 w, u32 h, VkFormat format, VkImageUsageFlags usage) {
    if (s->nimg == ARRAY_COUNT(s->imgs)) return nullptr;
    VkImg* i = &s->imgs[s->nimg];
    if (!img_create(v, i, w, h, format, usage)) return nullptr;
    s->nimg++;
    return i;
}

void scratch_free(VkCtx* v, VkScratch* s) {
    vkDeviceWaitIdle(v->dev);
    for (u32 i = 0; i < s->nbuf; ++i) buf_destroy(v, &s->bufs[i]);
    for (u32 i = 0; i < s->nimg; ++i) img_destroy(v, &s->imgs[i]);
    s->nbuf = s->nimg = 0;
    vkResetDescriptorPool(v->dev, v->desc_pool, 0);
}

VkDescriptorSet desc_alloc(VkCtx* v, VkLayoutId layout) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = v->desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &v->set_layouts[layout];
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(v->dev, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    return set;
}

void desc_buffer(VkCtx* v, VkDescriptorSet set, u32 binding, const VkBuf* b) {
    VkDescriptorBufferInfo bi = { b->buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(v->dev, 1, &w, 0, nullptr);
}

void desc_image(VkCtx* v, VkDescriptorSet set, u32 binding, const VkImg* img, bool storage) {
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, img->view,
                                 storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(v->dev, 1, &w, 0, nullptr);
}

// ---- command helpers -------------------------------------------------------------------------------------

bool fence_wait(VkCtx* v, VkFence fence) {
    return vkWaitForFences(v->dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) == VK_SUCCESS;
}

void cmd_begin(VkCommandBuffer cb) {
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
}

VkCommandBuffer once_begin(VkCtx* v) {
    fence_wait(v, v->fence[0]);
    cmd_begin(v->cmd[0]);
    return v->cmd[0];
}

bool once_submit(VkCtx* v) {
    vkEndCommandBuffer(v->cmd[0]);
    vkResetFences(v->dev, 1, &v->fence[0]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v->cmd[0];
    return vkQueueSubmit(v->gfx_queue, 1, &si, v->fence[0]) == VK_SUCCESS && fence_wait(v, v->fence[0]);
}

void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkImageLayout old_layout,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}
