#pragma once
// Physical disks (\\.\PhysicalDriveN): identity, the volumes on them and their temperature sensors.
// Shared by the temperatures and disk health windows.
#include "core/types.h"

typedef void* HANDLE;

constexpr u32 PHYSDISK_MAX = 16;

struct PhysDisk {
    u32 number;                 // N in \\.\PhysicalDriveN
    char model[96];             // vendor + product
    char serial[48];
    char firmware[16];
    char bus[24];               // "NVMe", "SATA", "USB", ...
    u32 bus_type;               // STORAGE_BUS_TYPE
    bool virtual_disk;          // VHD / VHDX / other virtual disk
    char letters[32];           // "C: D:" volumes on this disk
    u64 size_bytes;
};

u32 physdisk_enum(PhysDisk* out, u32 max);
// Opens \\.\PhysicalDriveN. `read`: with read/write access (needed for SMART / NVMe logs; administrator),
// otherwise query-only access (enough for identity and temperature). INVALID_HANDLE_VALUE on failure.
HANDLE physdisk_open(u32 number, bool read);
// Drive's own temperature (NVMe composite / SATA sensor) and its warning threshold (0 if unknown).
bool physdisk_temperature(HANDLE h, f32* temp_c, f32* warn_c);
