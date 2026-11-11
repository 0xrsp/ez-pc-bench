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

// Where the disk sits in the device tree (SetupAPI, no admin needed). Slower than physdisk_enum (tens of ms).
struct DiskLocation {
    u16 usb_vid, usb_pid;           // USB enclosure / bridge (0 = not on USB)
    u8 pcie_gen, pcie_width;        // link of the PCIe controller the disk hangs off (NVMe: the drive itself)
    u8 pcie_gen_max, pcie_width_max;
    char controller[96];            // friendly name of the nearest PCI ancestor (storage controller)
};
bool physdisk_location(u32 number, DiskLocation* out);

// Partitions from IOCTL_DISK_GET_DRIVE_LAYOUT_EX (works on a query-only handle) with the volumes mounted
// on them. `style` gets "GPT", "MBR" or "RAW".
struct DiskPartition {
    u32 number;
    u64 offset, size;
    char type[24];                  // "EFI system", "Basic data", "Recovery", "0x07", ...
    char letter;                    // 0 = no drive letter
    char fs[16], label[40];
};
u32 physdisk_partitions(HANDLE h, u32 number, DiskPartition* out, u32 max, char style[8]);

// Storage adapter properties of the disk's port (query-only handle): bus major / minor version (e.g. NVMe 2.0,
// SATA 3.x when reported) and whether the adapter queues commands (NCQ for SATA). False if unavailable.
bool physdisk_adapter(HANDLE h, u16* bus_major, u16* bus_minor, bool* queueing);
// Windows' view of the disk (query-only handle): -1 = unknown for each.
void physdisk_flags(HANDLE h, i32* trim, i32* write_cache, i32* seek_penalty);
