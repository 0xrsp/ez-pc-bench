// Vulkan pipeline creation (gpu_ext.h): compute and graphics pipelines per second, cold (unique SEED
// specialization constant, no application cache) vs cached (same pipelines from a reloaded VkPipelineCache).
#include "gpu/gpu_vk_ext.h"
#include "core/results.h"
#include "core/timer.h"
#include <stdlib.h>
#include <string.h>

#include "cs_pso_spv.h"
#include "vs_pso_spv.h"
#include "ps_pso_spv.h"

struct PsoCtx {
    VkxPipe layout;                 // set layout + pipeline layout shared by all pipelines
    VkSampler sampler;
    VkShaderModule cs, vs, ps;
    u32 seeds[GE_PSO_MAX];
    f64 ms[GE_PSO_MAX];
};

static VkPipeline create_compute(VkCtx* v, PsoCtx* c, VkPipelineCache cache, u32 seed) {
    const VkSpecializationMapEntry map = { 0, 0, 4 };
    const VkSpecializationInfo spec = { 1, &map, 4, &seed };
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, c->cs, "cs_pso", &spec };
    ci.layout = c->layout.layout;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(v->dev, cache, 1, &ci, nullptr, &p);
    return p;
}

// A typical opaque-or-blended mesh pass: RGBA8 + D32, depth test, alpha blend, back-face culling.
static VkPipeline create_graphics(VkCtx* v, PsoCtx* c, VkPipelineCache cache, u32 seed) {
    const VkSpecializationMapEntry map = { 0, 0, 4 };
    const VkSpecializationInfo spec = { 1, &map, 4, &seed };
    const VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, c->vs, "vs_pso", &spec },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, c->ps, "ps_pso", &spec },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState att = {};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    ri.depthAttachmentFormat = VK_DEPTH_FORMAT;
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
    ci.layout = c->layout.layout;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(v->dev, cache, 1, &ci, nullptr, &p);
    return p;
}

struct PassResult {
    u32 count;
    f64 rate;           // pipelines per second
    f64 max_ms;         // slowest single creation
    bool ok;
};

// Creates (and destroys) pipelines with seeds[0..max) until the budget runs out; the first one always counts.
static PassResult run_pass(VkCtx* v, PsoCtx* c, bool graphics, VkPipelineCache cache, u32 max, f64 budget_s,
                           const char* phase, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1) {
    PassResult r = { 0, 0.0, 0.0, true };
    live_phase(live, phase, "PSO/s");
    f64 total = 0.0;
    const f64 t_start = timer_now();
    for (u32 i = 0; i < max && !cancel->load(std::memory_order_relaxed); ++i) {
        if (i && timer_now() - t_start >= budget_s) break;
        const f64 t0 = timer_now();
        const VkPipeline pipe = graphics ? create_graphics(v, c, cache, c->seeds[i]) : create_compute(v, c, cache, c->seeds[i]);
        const f64 ms = (timer_now() - t0) * 1000.0;
        if (!pipe) { r.ok = false; break; }
        vkDestroyPipeline(v->dev, pipe, nullptr);
        c->ms[i] = ms;
        total += ms;
        r.max_ms = ms > r.max_ms ? ms : r.max_ms;
        r.count = i + 1;
        live_sample(live, 1000.0 / ms);
        gpu_ext_progress(live, timer_now() - t_start, budget_s, p0, p1);
    }
    r.rate = total > 0.0 ? r.count / (total / 1000.0) : 0.0;
    return r;
}

void vk_ext_pso_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    PsoCtx* c = (PsoCtx*)calloc(1, sizeof(PsoCtx));
    if (!c) { res_fail(out, "out of memory"); return; }
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.maxLod = VK_LOD_CLAMP_NONE;
    bool ok = vkCreateSampler(v->dev, &si, nullptr, &c->sampler) == VK_SUCCESS;
    const VkxBinding b[] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr }, { 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr },
        { 9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr }, { 10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr },
        { 11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr }, { 16, VK_DESCRIPTOR_TYPE_SAMPLER, &c->sampler },
    };
    ok = ok && vkx_set_layout(v, &c->layout, b, ARRAY_COUNT(b));
    c->cs = ok ? vkx_module(v, g_cs_pso_spv, sizeof(g_cs_pso_spv)) : VK_NULL_HANDLE;
    c->vs = ok ? vkx_module(v, g_vs_pso_spv, sizeof(g_vs_pso_spv)) : VK_NULL_HANDLE;
    c->ps = ok ? vkx_module(v, g_ps_pso_spv, sizeof(g_ps_pso_spv)) : VK_NULL_HANDLE;
    ok = c->cs && c->vs && c->ps;

    const f64 budget = p->duration_ms / 1000.0 * 0.4;
    PassResult res[2][2] = {};     // [compute, graphics][cold, cached]
    for (u32 g = 0; g < 2 && ok && !cancel->load(std::memory_order_relaxed); ++g) {
        for (u32 i = 0; i < GE_PSO_MAX; ++i) c->seeds[i] = (u32)gpu_ext_pso_seed();
        // Cold: unique pipelines into an empty cache, which is then serialized like a game's pipeline cache file.
        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        VkPipelineCache cache = VK_NULL_HANDLE;
        ok = vkCreatePipelineCache(v->dev, &pci, nullptr, &cache) == VK_SUCCESS;
        if (!ok) break;
        const f32 base = g * 0.5f;
        res[g][0] = run_pass(v, c, g == 1, cache, GE_PSO_MAX, budget, g ? "Graphics, cold" : "Compute, cold", live, cancel,
                             base, base + 0.4f);
        size_t size = 0;
        vkGetPipelineCacheData(v->dev, cache, &size, nullptr);
        void* data = size ? malloc(size) : nullptr;
        if (data) vkGetPipelineCacheData(v->dev, cache, &size, data);
        vkDestroyPipelineCache(v->dev, cache, nullptr);
        // Cached: the same pipelines from a cache created from that data.
        pci.initialDataSize = data ? size : 0;
        pci.pInitialData = data;
        ok = res[g][0].ok && vkCreatePipelineCache(v->dev, &pci, nullptr, &cache) == VK_SUCCESS;
        free(data);
        if (!ok) break;
        res[g][1] = run_pass(v, c, g == 1, cache, res[g][0].count, 1e9, g ? "Graphics, cached" : "Compute, cached", live,
                             cancel, base + 0.4f, base + 0.5f);
        vkDestroyPipelineCache(v->dev, cache, nullptr);
        ok = res[g][1].ok;
    }

    if (c->cs) vkDestroyShaderModule(v->dev, c->cs, nullptr);
    if (c->vs) vkDestroyShaderModule(v->dev, c->vs, nullptr);
    if (c->ps) vkDestroyShaderModule(v->dev, c->ps, nullptr);
    vkx_pipe_destroy(v, &c->layout);
    if (c->sampler) vkDestroySampler(v->dev, c->sampler, nullptr);
    free(c);
    if (!ok) { res_fail(out, "pipeline creation failed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_VALUES);
    res_add(out, "Compute, cold", "PSO/s", res[0][0].rate);
    res_add(out, "Compute, cached", "PSO/s", res[0][1].rate);
    res_add(out, "Compute, cold, slowest", "ms", res[0][0].max_ms);
    res_add(out, "Graphics, cold", "PSO/s", res[1][0].rate);
    res_add(out, "Graphics, cached", "PSO/s", res[1][1].rate);
    res_add(out, "Graphics, cold, slowest", "ms", res[1][0].max_ms);
}
