#pragma once
// Extended system information (FUTURE.md section 9): what CPU-Z / GPU-Z / Speccy show beyond core/sysinfo.h.
//
// Three cost tiers, so starting the app stays fast and never wakes a sleeping laptop dGPU:
//  - g_cpux (sysinfo_cpu.cpp): CPUID, registry, SMBIOS, CPU sets. Filled by sysinfo_init.
//  - g_dev (sysinfo_devices.cpp, sysinfo_display.cpp): PnP device tree walk, EDID, network / audio lists and the
//    driverless GPU details (D3DKMT, devnode properties). Tens of ms; filled on first use by devices_ensure():
//    the System details window, the system report and results files (chipset).
//  - g_gpux (sysinfo_gpu.cpp): Vulkan instance + D3D12 device per adapter. Creating them powers up a sleeping
//    dGPU, so it only runs on request: the System details window's "Query GPU APIs" button, the CLI's
//    --sysinfo, or gpu_api_details_query() from a GPU suite's init (the device is awake there anyway).
#include "core/types.h"
#include <atomic>
#include <stdio.h>

// ---- CPU ------------------------------------------------------------------------------------------------

struct CpuCache {
    u8 level;
    char type;              // 'D' data, 'I' instruction, 'U' unified
    u8 ways;                // 0xFF = fully associative
    u16 line;               // bytes
    u32 size_kib;           // one instance
    u32 count;              // instances
};

struct CpuCore {
    u32 lp_first;           // first logical processor (group 0 numbering)
    u8 lp_count;
    u8 efficiency;          // EfficiencyClass: on hybrid CPUs higher = faster cores (P), lower = E cores
    u8 sched;               // SchedulingClass: preferred-core ranking (higher = preferred)
    u8 l3;                  // last-level cache instance (CCX / CCD / ring), 0xFF unknown
};

struct CpuDetails {
    char codename[32];      // "Hawk Point", "Raptor Lake"
    char microarch[24];     // "Zen 4", "Raptor Cove + Gracemont"
    u32 microcode;          // registry "Update Revision", 0 = unknown
    u32 base_mhz;           // nominal clock (registry ~MHz)
    u32 max_mhz;            // SMBIOS type 4 "max speed" (often the boost clock; vendor-filled), 0 = unknown
    u32 bus_mhz;            // SMBIOS external clock
    char socket[32];        // SMBIOS socket designation
    u32 cache_count;
    CpuCache caches[8];
    u32 core_count;
    CpuCore cores[64];
    u32 l3_count;           // last-level cache instances (CCDs / CCXs)
    u32 perf_cores, eff_cores;  // hybrid split by EfficiencyClass (all cores are "perf" on non-hybrid CPUs)
    bool hybrid;
    u32 isa_count;
    const char* isa[128];   // CPUID feature flags, lowercase Linux-style names (static strings)
};
extern CpuDetails g_cpux;
void cpu_details_query(CpuDetails* d);

// ---- devices (lazy) --------------------------------------------------------------------------------------

constexpr u32 DEV_MAX_PCI = 160, DEV_MAX_USB = 96, DEV_MAX_NET = 24, DEV_MAX_AUDIO = 24, DEV_MAX_DISPLAYS = 8;

struct PciDev {
    u16 vendor, device;
    u16 sub_vendor, sub_device;
    u8 rev;
    u8 base_class, sub_class, prog_if;      // 0xFF = unknown
    u32 bus, dev, fn;                       // 0xFFFFFFFF = unknown
    u8 depth;                               // nesting below the root bus (bridges add a level)
    u8 link_gen, link_width, link_gen_max, link_width_max;     // PCIe, 0 = not reported
    char name[96];
};

struct UsbDev {
    u16 vid, pid;
    u8 depth;               // 0 = host controller, 1 = root hub, 2.. = devices behind hubs
    bool hub, controller;
    char name[96];
};

struct NetAdapter {
    char name[96];          // adapter description
    char type[16];          // "Ethernet", "Wi-Fi", ...
    bool up;
    u64 link_bps;           // receive link speed, 0 = unknown
};

struct AudioDev {
    char name[128];
    bool capture;           // false = playback
    bool is_default;
    u32 sample_rate, bits, channels;    // shared-mode mix format, 0 = unknown
};

struct DisplayInfo {
    char name[64];          // EDID monitor name, or the friendly name Windows has
    char maker[4];          // EDID PNP id, "BOE"
    u16 product;
    char connector[24];     // "Internal", "HDMI", "DisplayPort", ...
    char adapter[128];      // GPU that drives it
    u32 width, height;      // current mode
    f32 refresh_hz;         // current mode
    u32 native_w, native_h; // EDID preferred timing
    f32 native_hz;
    u16 vrr_min_hz, vrr_max_hz;     // EDID range limits / Adaptive-Sync range, 0 = not reported
    f32 size_in;            // diagonal from EDID image size, 0 = unknown
    i8 hdr_supported, hdr_on;       // -1 unknown
    u16 edid_year;
};

// Driverless GPU details (no device created): driver date and provider from the devnode, WDDM version, chip /
// BIOS strings and the driver's maximum memory clock / bandwidth from D3DKMT.
struct GpuDevInfo {
    char driver_date[16];   // "2026-10-02"
    char provider[48];
    char chip[64];          // KMT adapter registry info: chip type
    char bios[64];
    char wddm[8];           // "3.2"
    u64 mem_bw_max;         // bytes/s, 0 = not reported
    u64 pcie_bw_max;        // bytes/s
    u32 mem_mhz_max;        // memory clock as the driver reports it
    u8 link_gen, link_width, link_gen_max, link_width_max;
    bool integrated;
};

struct DeviceInfo {
    bool ready;
    char chipset[96];       // platform / chipset name ("AMD Phoenix (SoC, no separate chipset)")
    char host_bridge[96];
    u32 pci_count;
    PciDev pci[DEV_MAX_PCI];
    u32 usb_count;
    UsbDev usb[DEV_MAX_USB];
    u32 net_count;
    NetAdapter net[DEV_MAX_NET];
    u32 audio_count;
    AudioDev audio[DEV_MAX_AUDIO];
    u32 display_count;
    DisplayInfo displays[DEV_MAX_DISPLAYS];
    GpuDevInfo gpus[8];     // parallel to g_sys.gpus
    i8 tearing;             // DXGI_FEATURE_PRESENT_ALLOW_TEARING (needed for VRR in windowed games), -1 unknown
    i8 vrr_optimizations;   // Windows "Optimizations for windowed games" / VRR setting, -1 unknown
};
extern DeviceInfo g_dev;
void devices_ensure();      // gathers g_dev once (UI or worker thread; not thread-safe against itself)
void devices_refresh();     // gathers again
// sysinfo_devices.cpp internals shared with sysinfo_display.cpp
void displays_query(DeviceInfo* d);
const char* pci_class_name(u8 base, u8 sub);

// ---- GPU API details (on request; wakes the GPU) ------------------------------------------------------------

struct GpuApiInfo {
    // Vulkan
    bool vk;
    char vk_api[16];        // "1.4.312"
    char vk_driver[64];     // driverName + driverInfo
    u32 vk_ext_count;
    char vk_exts[8192];     // space-separated extension names
    u32 compute_units;      // AMD CUs (shader core properties 2), NVIDIA SMs, 0 = unknown
    const char* cu_name;    // "CUs", "SMs"
    u32 subgroup;           // wave / warp size
    u64 vram_heap;          // largest DEVICE_LOCAL heap
    u64 bar_heap;           // DEVICE_LOCAL | HOST_VISIBLE heap size (256 MiB = no Resizable BAR)
    i8 rebar;               // -1 unknown / integrated
    // D3D12
    bool d3d12;
    char feature_level[8];  // "12_2"
    char shader_model[8];   // "6.8"
    u8 raytracing_tier;     // 10 = 1.0, 11 = 1.1, 0 = none
    u8 mesh_tier, vrs_tier, sampler_feedback_tier, binding_tier;
    i8 upload_heaps;        // OPTIONS16 GPUUploadHeapSupported (needs Resizable BAR), -1 unknown
};
extern GpuApiInfo g_gpux[8];                    // parallel to g_sys.gpus
extern std::atomic<u32> g_gpux_ready;           // bit per GPU, set (release) after its entry is complete
void gpu_api_details_query(u32 gpu);            // Vulkan + D3D12 queries for one adapter (blocking, ~100-500 ms)
void gpu_api_details_query_all();

// ---- system report (sysreport.cpp) --------------------------------------------------------------------------
// Everything above plus core/sysinfo.h, as sections of key / value lines and tables. One model, three views:
// the System details window, plain text (clipboard / .txt) and TOON (.toon.txt, and a subset in results files).

enum RepKind : u8 { REP_SECTION, REP_KV, REP_TABLE, REP_ROW, REP_NOTE };
struct RepItem {
    RepKind kind;
    u8 cells;               // KV: 2, TABLE / ROW: column count, SECTION: 1
    bool in_results;        // SECTION: also written into results files
    u32 text;               // offset of the NUL-separated strings in SysReport::text
};
struct SysReport {
    u32 count, used;
    RepItem items[4096];
    char text[256 * 1024];
};
void sysreport_build(SysReport* r);             // calls devices_ensure(); includes g_gpux entries that are ready
const char* rep_cell(const SysReport* r, const RepItem* it, u32 cell);
void sysreport_text(const SysReport* r, char* out, u32 size);  // plain text, aligned columns
void sysreport_toon(const SysReport* r, FILE* f, u32 depth, bool results_only);
bool sysreport_save(const wchar_t* base_path);  // writes <base>.txt and <base>.toon.txt
void sysreport_write_results(FILE* f, u32 depth);    // the `details:` block of a results file (results.cpp)
