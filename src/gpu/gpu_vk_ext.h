#pragma once
// Vulkan side of the extra tests (gpu_ext.h). Each test creates its own layouts / pipelines / descriptor pool
// and destroys them afterwards, so the base backend's tables stay as they are.
// gpu_vk_ext.cpp: helpers, device feature hook, compute tests; gpu_vk_memory.cpp, gpu_vk_pso.cpp,
// gpu_vk_texture.cpp: the other groups.
#include "gpu/gpu_ext.h"
#include "gpu/gpu_vk_internal.h"

struct VkxBinding {
    u32 binding;
    VkDescriptorType type;
    const VkSampler* immutable;     // samplers only
};

struct VkxPipe {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
};

VkShaderModule vkx_module(VkCtx* v, const u8* spv, u32 size);
bool vkx_set_layout(VkCtx* v, VkxPipe* p, const VkxBinding* b, u32 nb);        // dsl + layout (push constants)
// Compute pipeline; subgroup_size != 0 requests that subgroup size when the device allows it.
bool vkx_compute_pipe(VkCtx* v, VkxPipe* p, const u8* spv, u32 size, const char* entry, const VkxBinding* b,
                      u32 nb, const VkSpecializationInfo* spec = nullptr, u32 subgroup_size = 0);
void vkx_pipe_destroy(VkCtx* v, VkxPipe* p);
VkDescriptorPool vkx_pool(VkCtx* v);
VkDescriptorSet vkx_set(VkCtx* v, VkDescriptorPool pool, const VkxPipe* p);

struct VkxDispatch {
    const VkxPipe* pipe;
    VkDescriptorSet set;
    GpuParams pc;
    u32 gx, gy;
};

// Warm-up, then timed batches (GPU timestamps around every dispatch, ~20 ms per batch) for `seconds`.
// Dispatch times (ms) go to ms[] (at most max); returns their count, or -1 on GPU error.
i32 vkx_timed(VkCtx* v, const VkxDispatch* d, f64 warm, f64 seconds, f64 work, const char* phase, const char* unit,
              f64 unit_div, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1, f64* ms, u32 max);
bool vkx_once(VkCtx* v, const VkxDispatch* d);       // one untimed dispatch, waits for it

struct VkxImage {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
};
bool vkx_image(VkCtx* v, VkxImage* img, u32 size, u32 mips, VkFormat format);   // sampled + transfer dst
// Copies mips levels from staging (level i at offsets[i], tightly packed) and leaves SHADER_READ_ONLY.
bool vkx_image_upload(VkCtx* v, const VkxImage* img, u32 size, u32 mips, const VkBuf* staging, const u64* offsets);
void vkx_image_destroy(VkCtx* v, VkxImage* img);

void vk_ext_memory_test(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                        const CancelFlag* cancel);
void vk_ext_pso_test(VkCtx* v, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel);
void vk_ext_texture_test(VkCtx* v, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                         const CancelFlag* cancel);
