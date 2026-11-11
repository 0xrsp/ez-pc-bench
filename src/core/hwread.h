#pragma once
// Driverless hardware readers shared by the run monitor (monitor.*) and the Sensors window (sensors.*).
//
// CPU: per-LP utilization from SystemProcessorPerformanceInformation (direct kernel query, < 1 ms) and
// effective clock from PDH "% Processor Performance" (the kernel's APERF/MPERF figure) x the nominal clock.
// That PDH query is served by OS worker threads that get little CPU while HIGHEST-priority benchmark threads
// fill every core: 0.1 ms idle, 120-260 ms under full load. Timing our own add chain instead was tried and
// rejected: with the SMT sibling busy (imul) it read 1.2 GHz for a 2.45 GHz core. Processor group 0 only.
//
// GPU: clocks, temperature, fan and power from D3DKMT perf data (what Task Manager uses); engine utilization
// and dedicated memory in use from PDH "GPU Engine" / "GPU Adapter Memory" (in-process, fast under load);
// PCIe link speed / width from the device's SetupAPI properties.
#include "core/types.h"

constexpr u32 HW_MAX_LPS = 64;

struct CpuReader {
    void* query;                // PDH_HQUERY
    void* perf;                 // PDH_HCOUNTER "% Processor Performance"
    f32 base_mhz;
    f64 collect_t;
    u8 prev[HW_MAX_LPS * 48];   // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION[HW_MAX_LPS]
    bool have_prev;
    u8* buf;                    // PDH array scratch
};

void cpu_reader_open(CpuReader* r);
void cpu_reader_close(CpuReader* r);
// Per-LP utilization (%) and effective clock (MHz, NaN when not known this time: first call, interval < 100 ms,
// implausible rate). Returns the LP count, 0 on the first call or failure.
u32 cpu_reader_sample(CpuReader* r, f32* util, f32* mhz);

struct GpuReading {
    f32 core_mhz, core_max_mhz;     // engine 0 (3D); NaN where the driver doesn't report it (NVIDIA idle)
    f32 mem_mhz, mem_max_mhz;
    f32 util;                       // busiest engine, %
    f32 temp_c, fan_rpm, power_pct;
    f32 vram_used_mib;              // dedicated memory in use, all processes
    u8 link_gen, link_width, link_gen_max, link_width_max;    // PCIe; 0 = unknown
};

struct GpuReader {
    u32 count;
    u32 handle[8];                  // D3DKMT_HANDLE per GPU (0 = not opened)
    u32 luid_low[8];
    i32 luid_high[8];
    u32 bus[8], address[8];         // PCI location, to find the device node for the link properties
    f32 warn_c[8];                  // driver warning temperature (or a default)
    void* query;                    // PDH_HQUERY
    void* engine;                   // PDH_HCOUNTER "GPU Engine(*)\Utilization Percentage"
    void* memory;                   // PDH_HCOUNTER "GPU Adapter Memory(*)\Dedicated Usage"
    u8* buf;
};

// Opens the GPUs of g_sys.gpus (indices match); only_gpu < count opens just that one.
void gpu_reader_open(GpuReader* r, u32 only_gpu = 0xFFFFFFFFu);
void gpu_reader_close(GpuReader* r);
void gpu_reader_collect(GpuReader* r);     // once per sample, before gpu_reader_read
void gpu_reader_read(GpuReader* r, u32 gpu, GpuReading* out, bool with_link);
