#pragma once
// Live hardware temperatures for the temperatures window, sampled once a second by a background thread
// while started:
//   - CPU / system: ACPI thermal zones ("Thermal Zone Information" performance counters). On most laptops
//     one of them tracks the CPU package; exact per-core temperatures need a kernel driver, which we don't use.
//   - GPUs: core temperature, fan speed and power from D3DKMT adapter perf data (what Task Manager shows).
//   - Drives: the drive's own sensor via IOCTL_STORAGE_QUERY_PROPERTY (NVMe composite temperature).
#include "core/hwread.h"

constexpr u32 SENSOR_MAX = 32;
constexpr u32 SENSOR_HISTORY = 120;     // samples (seconds)

enum SensorGroup : u8 { SG_CPU, SG_GPU, SG_DISK, SG_COUNT };

struct SensorReading {
    SensorGroup group;
    char name[96];
    char detail[96];            // source / extra values ("ACPI \_TZ.TZ01", "fan 2100 RPM, power 35%")
    bool valid;                 // has a current temperature
    f32 temp_c, min_c, max_c;   // min / max since the window opened
    f32 warn_c;                 // warning threshold (driver-reported or a sensible default)
    f32 history[SENSOR_HISTORY];    // oldest first, history_count valid entries
    u32 history_count;
    char note[160];             // why there's no value, if !valid
};

// Live clocks and load (Sensors window), sampled on the same thread.
struct CpuLive {
    u32 lps;                                // 0 until the second sample
    f32 util[HW_MAX_LPS], mhz[HW_MAX_LPS];  // per logical processor; mhz NaN = not reported this time
    f32 avg_util;                           // all LPs
    f32 busy_mhz;                           // utilization-weighted clock of the LPs >= 50 % busy (NaN if none)
    f32 max_mhz;                            // highest LP clock since the window opened
    f32 util_hist[SENSOR_HISTORY];
    u32 hist_count;
};

struct GpuLive {
    char name[128];
    bool opened;                            // D3DKMT adapter handle available
    GpuReading r;
    f32 vram_total_mib;
    f32 util_hist[SENSOR_HISTORY];
    u32 hist_count;
};

void sensors_start();           // idempotent
void sensors_stop();
// Copies the latest readings; returns how many. `updated` (optional): seconds since the last sample.
u32 sensors_snapshot(SensorReading* out, u32 max, f64* updated);
// Copies the live CPU / GPU readings; returns the GPU count (gpus: room for 8).
u32 sensors_live(CpuLive* cpu, GpuLive* gpus);
