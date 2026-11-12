#pragma once
// Advanced sensors through PawnIO (FUTURE.md section 8), opt-in: only when the user has installed the signed
// PawnIO driver (https://pawnio.eu) themselves and kept "Advanced sensors" on in Settings. We never install
// drivers and only read: MSRs, AMD SMN registers, the SMU's PM table, SMBus reads of the DIMM sensors / SPD.
// Without PawnIO every function here is a cheap no-op and the app behaves exactly as without this file.
//
// UNTESTED against the real driver: PawnIO wasn't installed on the development machine. Register sources and
// the test status are in docs/notes/pawnio.md.
//
// One sampling thread (adv_start / adv_stop, reference counted) reads once per second:
//   - every tick: package temperature (AMD Tctl via SMN / Intel package DTS), RAPL energy, the SMU PM table
//     (AMD; at most 1 Hz, AMD's guidance for its own monitoring SDK);
//   - only while no benchmark runs: per-logical-processor APERF/MPERF clocks and per-core Intel temperatures
//     (they hop the thread across every core), and the DIMM temperature sensors on the SMBus (every 2 s).
#include "core/hwread.h"

constexpr const char* ADV_PAWNIO_URL = "https://pawnio.eu";
// The official installer, latest release. The GUI's "Download & install" button (Settings) fetches it; that click
// is the app's only network access.
constexpr const wchar_t* ADV_PAWNIO_SETUP_URL = L"https://github.com/namazso/PawnIO.Setup/releases/latest/download/PawnIO_setup.exe";
constexpr u32 ADV_MAX_CCDS = 16;
constexpr u32 ADV_MAX_CORES = 64;
constexpr u32 ADV_MAX_DIMMS = 8;

enum AdvStatus : u8 {
    ADV_NOT_INSTALLED,      // no PawnIO driver
    ADV_NOT_RUNNING,        // installed, but its device isn't there (driver not started, reboot pending)
    ADV_OFF,                // switched off in Settings (or --no-adv-sensors)
    ADV_NEEDS_ADMIN,        // the device only opens for administrators
    ADV_NO_MODULES,         // driver there, but the signed module files aren't (see docs/notes/pawnio.md)
    ADV_UNSUPPORTED,        // no module accepted this CPU
    ADV_ACTIVE,
};

struct AdvInfo {
    AdvStatus status;
    char driver_version[24];    // "2.2.0" from the PawnIO uninstall entry, "" if unknown
    char modules[96];           // loaded modules, "AMDFamily17, RyzenSMU, SmbusPIIX4"
    char detail[192];           // why not active / what is missing
    char smbus_note[160];       // SMBus / DIMM sensor state ("locked by the embedded controller", ...)
    u32 pm_table_version;       // AMD SMU PM table version, 0 = none
    bool pm_table_mapped;       // we know that version's layout
};

enum AdvLimit : u32 {           // Intel IA32_(PACKAGE_)THERM_STATUS "status" bits: limiting right now
    ADV_LIM_THERMAL  = 1u << 0,
    ADV_LIM_PROCHOT  = 1u << 1,
    ADV_LIM_POWER    = 1u << 2,
    ADV_LIM_CURRENT  = 1u << 3,
    ADV_LIM_CROSS    = 1u << 4, // cross-domain (e.g. graphics) limit
    ADV_LIM_CRITICAL = 1u << 5,
};

struct AdvCpu {
    f64 time;                   // timer_now() of the sample; 0 = no sample yet
    f32 temp_c;                 // CPU temperature to watch: AMD Tdie (= Tctl minus the part's offset), Intel package
    f32 tctl_c;                 // AMD Tctl (fan-control value, may carry an offset); NaN on Intel
    u32 ccd_count;
    f32 ccd_c[ADV_MAX_CCDS];    // AMD per-CCD temperature (desktop / server parts; APUs have none)
    u32 core_count;
    f32 core_c[ADV_MAX_CORES];  // per physical core: SMU PM table (AMD, mapped versions) / DTS (Intel, while idle)
    f32 core_mhz[ADV_MAX_CORES];// AMD PM table per-core clock (mapped versions); NaN where unknown
    u32 lp_count;               // APERF/MPERF readings below (0 while a benchmark runs)
    f32 lp_mhz[HW_MAX_LPS];     // clock while running (APERF/MPERF x MPERF rate)
    f32 lp_busy[HW_MAX_LPS];    // % of the interval in C0 (MPERF rate x time)
    f32 stretch_pct;            // AMD clock stretching: worst (requested - effective) / requested of busy cores; NaN
    f32 package_w;              // RAPL package power
    f32 cores_w, gfx_w;         // Intel PP0 / PP1 (NaN on AMD)
    f32 vcore_v, vsoc_v;        // AMD SVI2 telemetry (Zen 1-3) or PM table; NaN where unknown
    f32 ppt_w;                  // AMD PM table socket power / PPT (mapped versions)
    f32 fclk_mhz, uclk_mhz, mclk_mhz;   // AMD PM table (mapped versions)
    u32 limits;                 // AdvLimit, Intel only
};

struct AdvDimm {
    u8 addr;                    // SMBus address of the sensor (0x50-0x57 SPD5118 hub, 0x18-0x1F DDR4 TSE2004)
    bool ddr5;
    bool valid;
    f32 temp_c;
};

struct AdvMem {
    f64 time;
    u32 count;
    AdvDimm dimms[ADV_MAX_DIMMS];
};

// Startup (GUI: after the settings are loaded; CLI: after the options): detects PawnIO, loads the modules if
// enabled, reads SPD / memory timings once (sysinfo_spd.cpp). Cheap and silent when PawnIO is absent.
void adv_init();
void adv_shutdown();
void adv_set_enabled(bool on);  // Settings switch (default on); turning it off closes the driver handles
void adv_redetect();            // detect PawnIO again (after its installer ran) and reopen the modules
bool adv_enabled();
AdvStatus adv_status(AdvInfo* info);    // info optional
const char* adv_status_text(AdvStatus s);  // "on", "off", "not installed", ...

void adv_start();               // reference counted; the first user starts the sampling thread (if active)
void adv_stop();
bool adv_cpu_snapshot(AdvCpu* out);     // false when nothing has been sampled
bool adv_mem_snapshot(AdvMem* out);
// Latest CPU temperature (AdvCpu::temp_c) if sampled within max_age_s, else NaN. For the run monitor and the
// stability tester's safety limit; call adv_start() first.
f32 adv_cpu_temp(f32 max_age_s);

// CLI --adv-report: status, one sample, SPD, timings and a raw PM table dump (to map new table versions).
void adv_print_report();
