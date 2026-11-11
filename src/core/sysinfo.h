#pragma once
// Static system information, gathered once at startup.
#include "core/types.h"

struct CpuInfo {
    char vendor[16];
    char brand[64];
    u32 family, model, stepping;
    u32 cores, threads;
    u32 l1d_kib, l2_kib, l3_kib;    // per core (L1/L2), total (L3)
    bool sse42, avx, avx2, fma, avx512f, avx512vl, avx512bw, avx512dq, avx512vnni, sha, aes, vaes;
};

struct RamModule {
    char type[16];          // "DDR5"
    char manufacturer[32];
    char part[32];
    char locator[32];
    u64 size_bytes;
    u32 speed_mts, configured_mts;
};

struct RamInfo {
    u64 total_bytes;        // usable by the OS
    u64 installed_bytes;    // physically installed
    u32 module_count;
    RamModule modules[8];
};

struct GpuInfo {
    char name[128];
    u64 dedicated_vram;
    u64 shared_mem;
    u32 vendor_id, device_id;
    u32 luid_low; i32 luid_high;
    bool software;
};

enum PowerMode : u8 { POWER_MODE_UNKNOWN, POWER_MODE_EFFICIENCY, POWER_MODE_BALANCED, POWER_MODE_PERFORMANCE };

struct PowerInfo {
    bool on_ac;
    bool has_battery;
    u8 battery_percent;     // 255 = unknown
    char scheme_name[64];   // active power plan
    bool scheme_high_perf;  // High / Ultimate performance plan
    PowerMode mode;         // Windows "power mode" overlay
};

struct DriveInfo {
    char letter;            // 'C'
    char model[64];         // vendor + product id of the backing device
    char bus[24];           // "NVMe", "SATA", "File-backed virtual", ...
    char fs[16];            // "NTFS", "ReFS"
    u32 bus_type;           // STORAGE_BUS_TYPE
    u32 logical_sector, physical_sector;
    u64 total_bytes, free_bytes;
    bool virtual_disk;      // VHD/VHDX or other virtual disk (BusTypeVirtual / BusTypeFileBackedVirtual)
    bool dev_drive;         // Windows Dev Drive (FSCTL_QUERY_PERSISTENT_VOLUME_STATE)
    bool refs;
    i32 bitlocker;          // raw System.Volume.BitLockerProtection, -1 = unknown
    bool bitlocker_on;
};

struct SysInfo {
    CpuInfo cpu;
    RamInfo ram;
    u32 gpu_count;
    GpuInfo gpus[8];
    u32 drive_count;
    DriveInfo drives[26];   // fixed drives
    PowerInfo power;
    char os[64];
    bool elevated;
};

extern SysInfo g_sys;

void sysinfo_init();
void sysinfo_refresh_power();
const char* power_mode_name(PowerMode m);
bool power_is_best_performance(const PowerInfo* p);
u32 sysinfo_default_gpu();      // most powerful adapter index (DXGI high-performance order)

// Device, bus, file system and sector sizes of one drive (no BitLocker). False if the drive doesn't exist.
bool drive_query(wchar_t letter, DriveInfo* d);
const DriveInfo* sysinfo_find_drive(wchar_t letter);   // fixed drives gathered at startup, null if unknown
// One-line description: "Samsung XYZ, NVMe, NTFS, 953 GiB (120 GiB free), sector 512/4096, BitLocker on".
void drive_describe(const DriveInfo* d, char* buf, u32 size);
// Disk-target warnings shared by GUI and CLI (VHD / Dev Drive, BitLocker). Returns the line count (0..2).
u32 drive_target_warnings(wchar_t letter, char lines[2][192]);
