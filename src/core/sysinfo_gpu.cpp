// GPU API details (sysinfo_ext.h): Vulkan version, driver, extensions, compute units, Resizable BAR; D3D12 feature
// level, shader model and feature tiers. Creating the Vulkan instance / D3D12 device powers up a sleeping laptop
// dGPU, so this only runs on request (System details window, CLI --sysinfo, or a GPU suite's init).
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>

GpuApiInfo g_gpux[8];
std::atomic<u32> g_gpux_ready;

static bool has_ext(const VkExtensionProperties* e, u32 n, const char* name) {
    for (u32 i = 0; i < n; ++i)
        if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

static void vk_query(const GpuInfo* gi, GpuApiInfo* g) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ez-pc-bench";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return;
    VkPhysicalDevice devs[16];
    u32 n = ARRAY_COUNT(devs);
    vkEnumeratePhysicalDevices(inst, &n, devs);
    for (u32 i = 0; i < n; ++i) {
        VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &sg };
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drv };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
        vkGetPhysicalDeviceProperties2(devs[i], &props);
        LUID luid;
        memcpy(&luid, id.deviceLUID, sizeof(luid));
        if (!id.deviceLUIDValid || luid.LowPart != gi->luid_low || luid.HighPart != gi->luid_high) continue;

        g->vk = true;
        const u32 v = props.properties.apiVersion;
        snprintf(g->vk_api, sizeof(g->vk_api), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
        snprintf(g->vk_driver, sizeof(g->vk_driver), "%s %s", drv.driverName, drv.driverInfo);
        g->subgroup = sg.subgroupSize;

        static VkExtensionProperties ext[1024];
        u32 ne = ARRAY_COUNT(ext);
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, ext);
        g->vk_ext_count = ne;
        u32 used = 0;
        for (u32 k = 0; k < ne; ++k) {
            const int w = snprintf(g->vk_exts + used, sizeof(g->vk_exts) - used, "%s%s", k ? " " : "", ext[k].extensionName);
            if (w < 0 || used + (u32)w >= sizeof(g->vk_exts)) break;
            used += (u32)w;
        }

        // Compute units from the vendor extensions (chained only when supported).
        VkPhysicalDeviceShaderCorePropertiesAMD amd = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD };
        VkPhysicalDeviceShaderCoreProperties2AMD amd2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_2_AMD };
        VkPhysicalDeviceShaderSMBuiltinsPropertiesNV nv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV };
        VkPhysicalDeviceProperties2 vp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        void** tail = &vp.pNext;
        const bool has_amd = has_ext(ext, ne, VK_AMD_SHADER_CORE_PROPERTIES_EXTENSION_NAME);
        const bool has_amd2 = has_ext(ext, ne, VK_AMD_SHADER_CORE_PROPERTIES_2_EXTENSION_NAME);
        const bool has_nv = has_ext(ext, ne, VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME);
        if (has_amd) { *tail = &amd; tail = &amd.pNext; }
        if (has_amd2) { *tail = &amd2; tail = &amd2.pNext; }
        if (has_nv) { *tail = &nv; tail = &nv.pNext; }
        if (vp.pNext) {
            vkGetPhysicalDeviceProperties2(devs[i], &vp);
            if (has_amd2 && amd2.activeComputeUnitCount) { g->compute_units = amd2.activeComputeUnitCount; g->cu_name = "CUs"; }
            else if (has_amd) {
                g->compute_units = amd.shaderEngineCount * amd.shaderArraysPerEngineCount * amd.computeUnitsPerShaderArray;
                g->cu_name = "CUs";
            } else if (has_nv) { g->compute_units = nv.shaderSMCount; g->cu_name = "SMs"; }
        }

        // Resizable BAR: the CPU-visible part of VRAM. Without ReBAR it's a 256 MiB window (NVIDIA: ~214 MiB).
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
        for (u32 h = 0; h < mp.memoryHeapCount; ++h)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > g->vram_heap)
                g->vram_heap = mp.memoryHeaps[h].size;
        const VkMemoryPropertyFlags bar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        for (u32 t = 0; t < mp.memoryTypeCount; ++t)
            if ((mp.memoryTypes[t].propertyFlags & bar) == bar) {
                const u64 size = mp.memoryHeaps[mp.memoryTypes[t].heapIndex].size;
                if (size > g->bar_heap) g->bar_heap = size;
            }
        g->rebar = props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? -1
                   : g->bar_heap > 512 * MiB                                         ? 1
                                                                                     : 0;
        break;
    }
    vkDestroyInstance(inst, nullptr);
}

static void d3d12_query(const GpuInfo* gi, GpuApiInfo* g) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return;
    IDXGIAdapter1* adapter = nullptr;
    LUID luid;
    luid.LowPart = gi->luid_low;
    luid.HighPart = gi->luid_high;
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        adapter->Release();
    }
    f->Release();
    if (!dev) return;
    g->d3d12 = true;
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { ARRAY_COUNT(levels), levels };
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        snprintf(g->feature_level, sizeof(g->feature_level), "%u_%u", (u32)fl.MaxSupportedFeatureLevel >> 12,
                 ((u32)fl.MaxSupportedFeatureLevel >> 8) & 0xF);
    // The runtime rejects shader models it doesn't know: start at the highest the header knows and step down.
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_HIGHEST_SHADER_MODEL };
    while (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) && sm.HighestShaderModel > D3D_SHADER_MODEL_5_1)
        sm.HighestShaderModel = (D3D_SHADER_MODEL)(sm.HighestShaderModel - 1);
    snprintf(g->shader_model, sizeof(g->shader_model), "%u.%u", (u32)sm.HighestShaderModel >> 4, (u32)sm.HighestShaderModel & 0xF);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) g->binding_tier = (u8)o.ResourceBindingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) g->raytracing_tier = (u8)o5.RaytracingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) g->vrs_tier = (u8)o6.VariableShadingRateTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)))) {
        g->mesh_tier = (u8)o7.MeshShaderTier;
        g->sampler_feedback_tier = (u8)o7.SamplerFeedbackTier;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = {};
    g->upload_heaps = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16)))
                          ? (o16.GPUUploadHeapSupported ? 1 : 0) : -1;
    dev->Release();
}

void gpu_api_details_query(u32 gpu) {
    if (gpu >= g_sys.gpu_count || gpu >= ARRAY_COUNT(g_gpux)) return;
    g_gpux_ready.fetch_and(~(1u << gpu), std::memory_order_acq_rel);
    GpuApiInfo* g = &g_gpux[gpu];
    memset(g, 0, sizeof(*g));
    g->rebar = g->upload_heaps = -1;
    vk_query(&g_sys.gpus[gpu], g);
    d3d12_query(&g_sys.gpus[gpu], g);
    g_gpux_ready.fetch_or(1u << gpu, std::memory_order_release);
}

void gpu_api_details_query_all() {
    for (u32 i = 0; i < g_sys.gpu_count; ++i) gpu_api_details_query(i);
}
