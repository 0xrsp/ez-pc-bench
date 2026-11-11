#pragma once
// Drive health for the disk health window. NVMe drives: the SMART / Health Information log (log page 2)
// via IOCTL_STORAGE_QUERY_PROPERTY (needs administrator). Other drives: the generic SMART "predict
// failure" flag (IOCTL_STORAGE_PREDICT_FAILURE) and the temperature sensor, when the driver provides them.
#include "core/physdisk.h"

enum HealthStatus : u8 { HEALTH_UNKNOWN, HEALTH_GOOD, HEALTH_WARNING, HEALTH_CRITICAL };

struct DiskHealth {
    PhysDisk disk;
    HealthStatus status;
    char status_text[200];          // why it isn't Good, or what's missing
    bool have_temp;
    f32 temp_c, warn_c;
    // NVMe health log
    bool have_nvme;
    u8 critical_warning;            // NVMe critical warning bits (0 = none)
    u8 spare_pct, spare_threshold_pct, used_pct;
    u64 data_read_bytes, data_written_bytes;
    u64 power_on_hours, power_cycles, unsafe_shutdowns, media_errors, error_log_entries;
    u32 warn_temp_minutes, crit_temp_minutes;
    // generic SMART
    bool have_predict;
    bool predict_failure;
};

// Reads every physical disk. Synchronous: a few IOCTLs per drive (milliseconds); don't call during disk tests.
u32 disk_health_scan(DiskHealth* out, u32 max);
