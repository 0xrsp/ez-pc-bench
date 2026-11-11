#pragma once
// Vulkan backend internals: device + pipelines + resource helpers (gpu_vk.cpp), tests (gpu_vk_tests.cpp).
#include "gpu/gpu_common.h"
#include <vulkan/vulkan.h>

// Descriptor set layouts (binding rules in gpu_common.h). VL_NONE = push constants only.
enum VkLayoutId : u8 {
    VL_NONE,        // raster without textures
    VL_BUF1,        // u0 storage buffer                       (alu, lds)
    VL_BUF2,        // u0 + t0 storage buffers                 (bandwidth)
    VL_DOWNSCALE,   // u0 storage image, t0 sampled image, s0  (preview)
    VL_TEX,         // t0..t3 sampled images, s0               (texfill, sprites)
    VL_COUNT
};

enum VkPipeId : u8 {
    VP_FP32, VP_FP16, VP_INT32, VP_BW_READ, VP_BW_WRITE, VP_BW_COPY, VP_LDS, VP_DOWNSCALE,
    VP_TRIANGLES, VP_DRAWCALL, VP_FILL, VP_TEXFILL, VP_SCENE, VP_SPRITE,
    VP_COUNT
};

enum MemKind : u8 { MEM_DEVICE, MEM_UPLOAD, MEM_READBACK };

constexpr VkFormat VK_RT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat VK_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr u32 VK_MAX_BATCH = 64;            // timed dispatches per command buffer (2 queries each)

struct GpuParams { u32 u[4]; f32 f[4]; };   // = struct Params in shaders/common.hlsli (32 bytes)

struct VkBuf {
    VkBuffer buf;
    VkDeviceMemory mem;
    u8* map;            // persistently mapped for host-visible kinds
    u64 size;
    bool coherent;
};

struct VkImg {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    u32 w, h;
};

// Push constants are visible to the stages we actually use. VK_SHADER_STAGE_ALL would also include
// tessellation / geometry / mesh / ray stages that some drivers then update on every vkCmdPushConstants.
constexpr VkShaderStageFlags VK_PC_STAGES = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct VkCtx {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkPhysicalDeviceLimits limits;
    u32 gfx_family, xfer_family;    // xfer_family == gfx_family when there is no dedicated transfer queue
    VkQueue gfx_queue, xfer_queue;
    f64 ts_period_ns;
    u64 ts_mask;
    bool fp16;                      // shaderFloat16 + shaderInt16
    // Device-level entry points for the per-draw hot loop: skips the vulkan-1.dll loader trampoline
    // (like D3D12's direct vtable calls), so the draw-call test measures the driver, not the loader.
    PFN_vkCmdPushConstants cmd_push_constants;
    PFN_vkCmdDraw cmd_draw;

    VkSampler sampler;              // s0: linear, wrap (immutable in the layouts)
    VkDescriptorSetLayout set_layouts[VL_COUNT];
    VkPipelineLayout layouts[VL_COUNT];
    VkPipeline pipes[VP_COUNT];
    VkPipelineLayout pipe_layout[VP_COUNT];     // = layouts[layout of that pipeline]
    VkDescriptorPool desc_pool;     // reset after every test

    VkCommandPool cmd_pool, xfer_pool;
    VkCommandBuffer cmd[GPU_FRAMES_IN_FLIGHT];
    VkCommandBuffer xfer_cmd;
    VkFence fence[GPU_FRAMES_IN_FLIGHT];
    VkFence xfer_fence;
    VkQueryPool query_pool;         // 2 * VK_MAX_BATCH timestamps

    // On-screen mode (gpu_present_vk.cpp): extensions found and enabled at instance / device creation.
    bool has_surface, has_swapchain, has_present_wait;
};

// Per-test resources, freed together by scratch_free.
struct VkScratch {
    VkBuf bufs[8];
    VkImg imgs[8];
    u32 nbuf, nimg;
};

VkBuf* scratch_buf(VkCtx* v, VkScratch* s, u64 size, VkBufferUsageFlags usage, MemKind kind);
VkImg* scratch_img(VkCtx* v, VkScratch* s, u32 w, u32 h, VkFormat format, VkImageUsageFlags usage);
void scratch_free(VkCtx* v, VkScratch* s);     // waits idle, destroys everything, resets the descriptor pool
void buf_destroy(VkCtx* v, VkBuf* b);
bool buf_create(VkCtx* v, VkBuf* b, u64 size, VkBufferUsageFlags usage, MemKind kind);
void buf_invalidate(VkCtx* v, const VkBuf* b);  // before host reads of non-coherent memory

VkDescriptorSet desc_alloc(VkCtx* v, VkLayoutId layout);
void desc_buffer(VkCtx* v, VkDescriptorSet set, u32 binding, const VkBuf* b);
void desc_image(VkCtx* v, VkDescriptorSet set, u32 binding, const VkImg* img, bool storage);

// One-shot work on the graphics queue (uses cmd[0] / fence[0]).
VkCommandBuffer once_begin(VkCtx* v);
bool once_submit(VkCtx* v);

bool fence_wait(VkCtx* v, VkFence fence);       // false on timeout / device lost
void cmd_begin(VkCommandBuffer cb);
void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkImageLayout old_layout,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout new_layout);
void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

// On-screen mode (gpu_present_vk.cpp, see gpu_present.h). One at a time.
u32 vkp_instance_extensions(VkCtx* v, const char** out);   // surface extensions (writes <= 2), sets has_surface
// Appends swapchain (+ present id / wait) to out[*n] and returns the feature chain to use in front of `next`.
void* vkp_device_extensions(VkCtx* v, const char** out, u32* n, void* next);
struct VkPresent;
VkPresent* vkp_open(VkCtx* v, const BenchParams* p, const char* test_name, char* err, u32 err_size);
bool vkp_acquire(VkPresent* s, VkCtx* v, u32 slot);         // next swapchain image, signals the slot's semaphore
// Blits `src` (left in its stage / access / layout) into the acquired image and moves that to PRESENT_SRC.
void vkp_record_copy(VkPresent* s, VkCommandBuffer cb, VkImage src, u32 src_w, u32 src_h, VkPipelineStageFlags2 stage,
                     VkAccessFlags2 access, VkImageLayout layout);
// Submits cb (waits for the acquire, signals `fence`) and presents. 0 ok, 1 window closed by the user, 2 error.
u32 vkp_submit_present(VkPresent* s, VkCtx* v, VkCommandBuffer cb, VkFence fence, u32 slot, bool measuring);
void vkp_begin_measure(VkPresent* s);
void vkp_close(VkPresent* s, VkCtx* v);
