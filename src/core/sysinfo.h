#pragma once
// Static system information, gathered once at startup.
#include "core/types.h"

struct CpuInfo {
    char vendor[16];
    char brand[64];
    u32 family, model, stepping;
    u32 cores, threads;
    u32 l1d_kib, l2_kib, l3_kib;    // per core (L1/L2), total (L3)
    bool sse42, popcnt, avx, avx2, fma, avx512f, avx512vl, avx512bw, avx512dq, avx512vnni, avx512bf16, sha, aes, vaes;
};

// The exes are built for x86-64-v2 (CMakeLists.txt). Checked first thing in main, before anything uses it.
bool cpu_baseline_ok();
// Dev (--cpu-baseline): after sysinfo_init, pretend the CPU has nothing beyond x86-64-v2 (no AVX / FMA / AVX-512)
// to try the gated tests and SSE fallbacks on a modern PC.
void sysinfo_cap_to_baseline();
#define CPU_BASELINE_MSG \
    "This program needs a CPU with SSE4.2 and POPCNT (Intel Nehalem / AMD Bulldozer, 2008-2011, or newer)."

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
    char driver[24];        // user-mode driver version, "32.0.16.2114"
    i8 hags;                // hardware-accelerated GPU scheduling: -1 unsupported, 0 off, 1 on
};

// SMBIOS types 0 / 1 / 2.
struct BoardInfo {
    char sys_vendor[64], sys_product[64];
    char board_vendor[64], board_product[64];
    char bios_vendor[64], bios_version[64], bios_date[16];
};

// Windows settings that move benchmark numbers. -1 = unknown, 0 = off, 1 = on.
struct WinState {
    i8 hypervisor;          // running under a hypervisor (Hyper-V / VBS root partition counts)
    i8 vbs, hvci;           // virtualization-based security, memory integrity (Core isolation)
    i8 secure_boot;
    i8 game_mode;
    u8 tpm;                 // 20 = 2.0, 12 = 1.2, 0 = none / unknown
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
    BoardInfo board;
    WinState win;
    char os[64];
    bool elevated;
};

extern SysInfo g_sys;

void sysinfo_init();
void sysinfo_refresh_power();
const char* power_mode_name(PowerMode m);
bool power_is_best_performance(const PowerInfo* p);
u32 sysinfo_default_gpu();      // most powerful adapter index (DXGI high-performance order)
// File-name friendly short names: "AMD Ryzen 7 250 w/ Radeon 780M Graphics" -> "r7-250",
// "13th Gen Intel(R) Core(TM) i7-13700H" -> "i7-13700h", "NVIDIA GeForce RTX 5060 Laptop GPU" -> "rtx5060-laptop",
// "AMD Radeon 780M Graphics" -> "radeon-780m".
void cpu_short_name(const char* brand, char* out, u32 size);
void gpu_short_name(const char* name, char* out, u32 size);

// Device, bus, file system and sector sizes of one drive (no BitLocker). False if the drive doesn't exist.
bool drive_query(wchar_t letter, DriveInfo* d);
const DriveInfo* sysinfo_find_drive(wchar_t letter);   // fixed drives gathered at startup, null if unknown
// One-line description: "Samsung XYZ, NVMe, NTFS, 953 GiB (120 GiB free), sector 512/4096, BitLocker on".
void drive_describe(const DriveInfo* d, char* buf, u32 size);
// Disk-target warnings shared by GUI and CLI (VHD / Dev Drive, BitLocker). Returns the line count (0..2).
u32 drive_target_warnings(wchar_t letter, char lines[2][192]);

// sysinfo_win.cpp
void winstate_query(WinState* w);
void gpu_details_query(SysInfo* s);     // driver version + HAGS per GPU
void winstate_describe(const WinState* w, char* buf, u32 size);
// Warnings about Windows settings that change results (memory integrity on, Game Mode off). Returns the count.
u32 winstate_warnings(const WinState* w, char lines[][192], u32 max);
