#pragma once
// Live hardware temperatures for the temperatures window, sampled once a second by a background thread
// while started:
//   - CPU / system: ACPI thermal zones ("Thermal Zone Information" performance counters). On most laptops
//     one of them tracks the CPU package; exact per-core temperatures need a kernel driver, which we don't use.
//   - GPUs: core temperature, fan speed and power from D3DKMT adapter perf data (what Task Manager shows).
//   - Drives: the drive's own sensor via IOCTL_STORAGE_QUERY_PROPERTY (NVMe composite temperature).
#include "core/types.h"

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

void sensors_start();           // idempotent
void sensors_stop();
// Copies the latest readings; returns how many. `updated` (optional): seconds since the last sample.
u32 sensors_snapshot(SensorReading* out, u32 max, f64* updated);
