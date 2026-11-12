#pragma once
// NVMe Identify Controller power states (disk_nvme.cpp).
#include "core/types.h"

struct NvmePowerState {
    u32 index;                  // PSn
    bool non_operational;       // the drive serves no I/O in this state; APST parks it here when idle
    f32 max_power_w;
    u32 entry_lat_us, exit_lat_us;  // advertised by the drive
};

// Power states of \\.\PhysicalDrive<phys_disk> (query-only handle, no admin). 0 if not NVMe or not answered.
u32 nvme_power_states(i32 phys_disk, NvmePowerState* out, u32 max, bool* apst_supported);
