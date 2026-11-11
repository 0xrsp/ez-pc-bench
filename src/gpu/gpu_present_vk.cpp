// Vulkan on-screen mode (gpu_present.h): VK_KHR_win32_surface swapchain on the benchmark window, present
// mode IMMEDIATE (tearing) or MAILBOX, display confirmation via VK_KHR_present_id + VK_KHR_present_wait.
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include "gpu/gpu_vk_internal.h"
#include "gpu/gpu_present.h"
#include "core/timer.h"
#include <stdio.h>
#include <string.h>

constexpr u32 MAX_IMAGES = 8;

struct VkPresent {
    PresentWindow win;
    VkSurfaceKHR surface;
    VkSwapchainKHR sc;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    VkImage images[MAX_IMAGES];
    u32 image_count, image;                     // image = acquired this frame
    VkSemaphore acquired[GPU_FRAMES_IN_FLIGHT];  // per frame slot
    VkSemaphore done[MAX_IMAGES];               // per swapchain image
    u64 present_id;                             // last present id used
    u64 wait_next;                              // next id to confirm with vkWaitForPresentKHR
    PFN_vkWaitForPresentKHR wait_for_present;   // null without VK_KHR_present_wait
};
static VkPresent s_present;

// ---- extension setup (called while creating the instance / device) --------------------------------

static bool has_ext(const VkExtensionProperties* props, u32 n, const char* name) {
    for (u32 i = 0; i < n; ++i)
        if (!strcmp(props[i].extensionName, name)) return true;
    return false;
}

u32 vkp_instance_extensions(VkCtx* v, const char** out) {
    u32 n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    static VkExtensionProperties props[256];
    if (n > ARRAY_COUNT(props)) n = ARRAY_COUNT(props);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, props);
    v->has_surface = has_ext(props, n, VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(props, n, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    if (!v->has_surface) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    return 2;
}

void* vkp_device_extensions(VkCtx* v, const char** out, u32* n, void* next) {
    static VkPhysicalDevicePresentIdFeaturesKHR id_feat;
    static VkPhysicalDevicePresentWaitFeaturesKHR wait_feat;
    v->has_swapchain = v->has_present_wait = false;
    if (!v->has_surface) return next;
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(v->phys, nullptr, &count, nullptr);
    static VkExtensionProperties props[512];
    if (count > ARRAY_COUNT(props)) count = ARRAY_COUNT(props);
    vkEnumerateDeviceExtensionProperties(v->phys, nullptr, &count, props);
    if (!has_ext(props, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return next;
    v->has_swapchain = true;
    out[(*n)++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (!has_ext(props, count, VK_KHR_PRESENT_ID_EXTENSION_NAME) || !has_ext(props, count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
        return next;
    id_feat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    wait_feat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, &id_feat };
    VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &wait_feat };
    vkGetPhysicalDeviceFeatures2(v->phys, &f);
    if (!id_feat.presentId || !wait_feat.presentWait) return next;
    v->has_present_wait = true;
    out[(*n)++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
    out[(*n)++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
    id_feat.pNext = next;
    return &wait_feat;
}

// ---- swapchain -------------------------------------------------------------------------------------

void vkp_close(VkPresent* s, VkCtx* v) {
    if (!s) return;
    vkDeviceWaitIdle(v->dev);
    for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i)
        if (s->acquired[i]) vkDestroySemaphore(v->dev, s->acquired[i], nullptr);
    for (u32 i = 0; i < MAX_IMAGES; ++i)
        if (s->done[i]) vkDestroySemaphore(v->dev, s->done[i], nullptr);
    if (s->sc) vkDestroySwapchainKHR(v->dev, s->sc, nullptr);
    if (s->surface) vkDestroySurfaceKHR(v->instance, s->surface, nullptr);
    present_window_close(&s->win);
    *s = {};
}

static VkPresent* open_fail(VkPresent* s, VkCtx* v, char* err, u32 err_size, const char* what, VkResult r) {
    if (r != VK_SUCCESS) snprintf(err, err_size, "%s (VkResult %d)", what, (int)r);
    else snprintf(err, err_size, "%s", what);
    vkp_close(s, v);
    return nullptr;
}

static bool blit_dst_ok(VkCtx* v, VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(v->phys, f, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

VkPresent* vkp_open(VkCtx* v, const BenchParams* p, const char* test_name, char* err, u32 err_size) {
    VkPresent* s = &s_present;
    *s = {};
    if (!v->has_surface || !v->has_swapchain) return open_fail(s, v, err, err_size, "Vulkan surface / swapchain extensions not available", VK_SUCCESS);
    if (!present_window_open(&s->win, p, "Vulkan", test_name, err, err_size)) { vkp_close(s, v); return nullptr; }

    VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = s->win.hwnd;
    VkResult r = vkCreateWin32SurfaceKHR(v->instance, &si, nullptr, &s->surface);
    if (r != VK_SUCCESS) return open_fail(s, v, err, err_size, "vkCreateWin32SurfaceKHR failed", r);
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(v->phys, v->gfx_family, s->surface, &supported);
    if (!supported) return open_fail(s, v, err, err_size, "this GPU's graphics queue can't present to the window", VK_SUCCESS);

    VkSurfaceCapabilitiesKHR caps;
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->phys, s->surface, &caps);
    if (r != VK_SUCCESS) return open_fail(s, v, err, err_size, "surface capabilities query failed", r);
    s->extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{ s->win.w, s->win.h };
    if (!s->extent.width || !s->extent.height || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return open_fail(s, v, err, err_size, "surface can't be a transfer destination", VK_SUCCESS);

    VkSurfaceFormatKHR formats[64];
    u32 nf = ARRAY_COUNT(formats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->phys, s->surface, &nf, formats);
    VkSurfaceFormatKHR fmt = {};
    for (u32 i = 0; i < nf && !fmt.format; ++i)
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            blit_dst_ok(v, formats[i].format))
            fmt = formats[i];
    if (!fmt.format) return open_fail(s, v, err, err_size, "no 8-bit UNORM surface format with blit support", VK_SUCCESS);

    VkPresentModeKHR modes[16];
    u32 nm = ARRAY_COUNT(modes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(v->phys, s->surface, &nm, modes);
    s->mode = VK_PRESENT_MODE_FIFO_KHR;     // always supported; vsync on, only if nothing better exists
    for (u32 i = 0; i < nm; ++i) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) s->mode = modes[i];
        else if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && s->mode != VK_PRESENT_MODE_IMMEDIATE_KHR) s->mode = modes[i];
    }

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = s->surface;
    ci.minImageCount = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = s->extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (0u - caps.supportedCompositeAlpha));
    ci.presentMode = s->mode;
    ci.clipped = VK_TRUE;
    r = vkCreateSwapchainKHR(v->dev, &ci, nullptr, &s->sc);
    if (r != VK_SUCCESS) return open_fail(s, v, err, err_size, "vkCreateSwapchainKHR failed", r);
    s->image_count = MAX_IMAGES;
    r = vkGetSwapchainImagesKHR(v->dev, s->sc, &s->image_count, s->images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return open_fail(s, v, err, err_size, "vkGetSwapchainImagesKHR failed", r);

    const VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < GPU_FRAMES_IN_FLIGHT; ++i)
        if (vkCreateSemaphore(v->dev, &sem, nullptr, &s->acquired[i]) != VK_SUCCESS)
            return open_fail(s, v, err, err_size, "vkCreateSemaphore failed", VK_SUCCESS);
    for (u32 i = 0; i < s->image_count; ++i)
        if (vkCreateSemaphore(v->dev, &sem, nullptr, &s->done[i]) != VK_SUCCESS)
            return open_fail(s, v, err, err_size, "vkCreateSemaphore failed", VK_SUCCESS);
    if (v->has_present_wait) s->wait_for_present = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(v->dev, "vkWaitForPresentKHR");
    return s;
}

// ---- per frame -------------------------------------------------------------------------------------

bool vkp_acquire(VkPresent* s, VkCtx* v, u32 slot) {
    const VkResult r = vkAcquireNextImageKHR(v->dev, s->sc, 2000000000ull, s->acquired[slot], VK_NULL_HANDLE, &s->image);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

void vkp_record_copy(VkPresent* s, VkCommandBuffer cb, VkImage src, u32 src_w, u32 src_h, VkPipelineStageFlags2 stage,
                     VkAccessFlags2 access, VkImageLayout layout) {
    const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImage dst = s->images[s->image];
    const VkPipelineStageFlags2 blit = VK_PIPELINE_STAGE_2_BLIT_BIT;
    image_barrier(cb, src, color, stage, access, layout, blit, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // Source stage = the stage the acquire semaphore is waited at, so the layout change waits for it.
    image_barrier(cb, dst, color, blit, 0, VK_IMAGE_LAYOUT_UNDEFINED, blit, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit region = {};
    region.srcSubresource = { color, 0, 0, 1 };
    region.srcOffsets[1] = { (i32)src_w, (i32)src_h, 1 };
    region.dstSubresource = { color, 0, 0, 1 };
    region.dstOffsets[1] = { (i32)s->extent.width, (i32)s->extent.height, 1 };
    const bool same = src_w == s->extent.width && src_h == s->extent.height;
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    image_barrier(cb, dst, color, blit, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    image_barrier(cb, src, color, blit, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, access, layout);
}

// Polls (timeout 0) which presents have reached the screen; the time logged is when the poll saw it.
static void poll_displayed(VkPresent* s, VkCtx* v) {
    if (!s->wait_for_present) return;
    while (s->wait_next && s->wait_next <= s->present_id && s->wait_for_present(v->dev, s->sc, s->wait_next, 0) == VK_SUCCESS)
        present_log_displayed(s->wait_next++, timer_now());
}

u32 vkp_submit_present(VkPresent* s, VkCtx* v, VkCommandBuffer cb, VkFence fence, u32 slot, bool measuring) {
    vkResetFences(v->dev, 1, &fence);
    const VkSemaphoreSubmitInfo wait = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, s->acquired[slot], 0,
                                         VK_PIPELINE_STAGE_2_BLIT_BIT, 0 };
    const VkSemaphoreSubmitInfo signal = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, s->done[s->image], 0,
                                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
    const VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cb, 0 };
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    if (vkQueueSubmit2(v->gfx_queue, 1, &si, fence) != VK_SUCCESS) return 2;

    const u64 id = ++s->present_id;
    VkPresentIdKHR pid = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr, 1, &id };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, s->wait_for_present ? &pid : nullptr };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s->done[s->image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s->sc;
    pi.pImageIndices = &s->image;
    const VkResult r = vkQueuePresentKHR(v->gfx_queue, &pi);
    const f64 t = timer_now();
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return 2;
    if (measuring) {
        present_log_presented(id, t);
        poll_displayed(s, v);
    }
    return present_window_pump(&s->win) ? 0 : 1;
}

void vkp_begin_measure(VkPresent* s) {
    present_log_begin(s->present_id + 1);
    s->wait_next = s->present_id + 1;
}
