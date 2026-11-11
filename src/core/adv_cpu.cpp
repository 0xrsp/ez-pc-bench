// CPU sensors through PawnIO (see adv_sensors.h). Reads only: MSRs from the modules' read allow-lists, AMD SMN
// registers, and the SMU PM table through RyzenSMU's own table functions (never its raw command / write calls).
//
// Register sources (cited, not copied):
//   - LibreHardwareMonitor (MPL-2.0), LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs and IntelCpu.cs:
//     Tctl / CCD SMN addresses and decoding, SVI2 plane offsets, RAPL handling;
//   - Linux drivers/hwmon/k10temp.c: Tctl range / TJ_SEL bits and the per-part Tdie offsets;
//   - AMD PPR 55570 / 57896 (family 19h / 1Ah): MSRC001_0064 P-state definitions, MSRC001_0293 current P-state,
//     MSRC001_0299 / 029B RAPL; Intel SDM vol. 4: IA32_THERM_STATUS, IA32_PACKAGE_THERM_STATUS, RAPL MSRs;
//   - PM table layouts: RyzenAdj (LGPL-3.0, lib/api.c) and LibreHardwareMonitor RyzenSMU.cs / Amd17Cpu.cs.
// UNTESTED against the real driver (PawnIO wasn't installed on the development machine).
#include "core/adv_internal.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <math.h>
#include <string.h>

// ---- registers ------------------------------------------------------------------------------------------

constexpr u32 SMN_THM_TCON_CUR_TMP = 0x00059800;   // [31:21] Tctl x 1/8 C; [19] range select or [17:16] = 3: -49 C
constexpr u32 SMN_SVI = 0x0005A000;                // SVI2 telemetry (Zen 1-3); +0x8 = TFN plane flags
constexpr u32 SMN_CCD_ZEN2 = 0x00059954;           // Matisse, Castle Peak, Vermeer: 8 CCDs, [11:0] x 1/8 C - 305 C
constexpr u32 SMN_CCD_ZEN4 = 0x00059B08;           // Raphael, Granite Ridge
constexpr u32 SMN_CCD_TURIN = 0x000599F0;          // Turin, Shimada Peak: 16 CCDs, [11] valid

constexpr u32 MSR_AMD_MPERF = 0xC00000E7, MSR_AMD_APERF = 0xC00000E8;   // read-only aliases
constexpr u32 MSR_AMD_PSTATE_0 = 0xC0010064;       // [63] enabled; Zen 1-4: FID [7:0], DfsId [13:8]; Zen 5: FID [11:0]
constexpr u32 MSR_AMD_HW_PSTATE = 0xC0010293;      // current hardware P-state, same FID / DfsId fields
constexpr u32 MSR_AMD_PWR_UNIT = 0xC0010299;       // [12:8] energy unit: 1 / 2^ESU J
constexpr u32 MSR_AMD_PKG_ENERGY = 0xC001029B;     // [31:0], wraps

constexpr u32 MSR_IA32_MPERF = 0xE7, MSR_IA32_APERF = 0xE8;
constexpr u32 MSR_PLATFORM_INFO = 0xCE;            // [15:8] max non-turbo ratio x 100 MHz (TSC / MPERF rate)
constexpr u32 MSR_IA32_THERM_STATUS = 0x19C;       // per core: [31] valid, [22:16] C below TjMax, status bits
constexpr u32 MSR_IA32_TEMPERATURE_TARGET = 0x1A2; // [23:16] TjMax
constexpr u32 MSR_IA32_PKG_THERM_STATUS = 0x1B1;
constexpr u32 MSR_RAPL_POWER_UNIT = 0x606;         // [12:8] energy unit
constexpr u32 MSR_PKG_ENERGY = 0x611, MSR_PP0_ENERGY = 0x639, MSR_PP1_ENERGY = 0x641;

// ---- PM table layouts -----------------------------------------------------------------------------------
// Byte offsets of float32 values, -1 = unknown. Core clocks may be GHz or MHz depending on the table; values
// below 20 are taken as GHz. Phoenix / Hawk Point (0x4C00xx, this project's laptop) only has the RyzenAdj-known
// fields: its per-core layout isn't public yet. `--adv-report` dumps the raw table to map more fields.
struct PmLayout {
    u32 version;
    u16 size;
    i16 tctl, ppt, vcore, vsoc, fclk, uclk, mclk;
    i16 core_temp, core_clk;    // first core; core n at +4n
    u8 cores;                   // per-core entries in the table
};

static const PmLayout s_pm_layouts[] = {
    // Raven / Picasso (LibreHardwareMonitor RyzenSMU.cs 0x1E0004; Tctl value from RyzenAdj, 0x58 is its limit)
    { 0x1E0004, 0x608, 0x5C, -1, -1, -1, 0x298, 0x2C4, 0x2C8, 0x1B0, -1, 4 },
    // Renoir / Lucienne (RyzenAdj)
    { 0x370000, 0x794, 0x44, 0x98, -1, 0x198, 0x5CC, -1, 0x5D4, 0x340, 0x3A0, 8 },
    { 0x370001, 0x884, 0x44, 0x98, -1, 0x198, 0x5CC, -1, 0x5D4, 0x340, 0x3A0, 8 },
    { 0x370002, 0x88C, 0x44, 0x98, -1, 0x198, 0x5CC, -1, 0x5D4, 0x340, 0x3A0, 8 },
    { 0x370003, 0x8AC, 0x44, 0x98, -1, 0x198, 0x5CC, -1, 0x5D4, 0x340, 0x3A0, 8 },
    { 0x370004, 0x8AC, 0x44, 0x98, -1, 0x198, 0x5CC, -1, 0x5D4, 0x340, 0x3A0, 8 },
    { 0x370005, 0x8C8, 0x44, 0x98, -1, 0x198, 0x5E8, -1, 0x5F0, 0x35C, 0x3BC, 8 },
    // Cezanne (RyzenAdj)
    { 0x400004, 0x944, 0x44, 0x98, -1, 0x19C, 0x664, -1, 0x66C, 0x360, 0x3C0, 8 },
    { 0x400005, 0x944, 0x44, 0x98, -1, 0x19C, 0x664, -1, 0x66C, 0x360, 0x3C0, 8 },
    // Phoenix / Hawk Point (RyzenAdj: Tctl value 0x44, fast PPT value 0x0C; per-core layout unknown)
    { 0x4C0003, 0xB18, -1, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    { 0x4C0004, 0xB1C, -1, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    { 0x4C0005, 0xAF8, -1, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    { 0x4C0006, 0xAFC, 0x44, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    { 0x4C0007, 0xB00, 0x44, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    { 0x4C0008, 0xAF0, 0x44, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    { 0x4C0009, 0xB00, 0x44, 0x0C, -1, -1, -1, -1, -1, -1, -1, 0 },
    // Strix Point (RyzenAdj)
    { 0x5D0008, 0xD54, 0x44, 0xD0, -1, -1, 0x4E0, -1, -1, 0xA38, 0xA68, 12 },
    { 0x5D0009, 0xD54, 0x44, 0xD0, -1, -1, 0x4E0, -1, -1, 0xA38, 0xA68, 12 },
    { 0x5D000B, 0xD54, 0x44, 0xD0, -1, -1, 0x4E0, -1, -1, 0xA38, 0xA68, 12 },
    // Strix Halo (RyzenAdj)
    { 0x64020C, 0xE50, 0x5C, -1, -1, -1, -1, -1, -1, 0xC10, 0xC50, 16 },
    // Matisse / Vermeer (LibreHardwareMonitor: fabric 48, uncore 50, memory 51)
    { 0x240903, 0x518, -1, -1, -1, -1, 0xC0, 0xC8, 0xCC, -1, -1, 0 },
    { 0x380805, 0x8F0, -1, -1, -1, -1, 0xC0, 0xC8, 0xCC, -1, -1, 0 },
    // Raphael (LibreHardwareMonitor: PPT 3, package 11, VDDCR 47, SoC 52, fabric 70, uncore 74, memory 78)
    { 0x540004, 0x948, 0x2C, 0x0C, 0xBC, 0xD0, 0x118, 0x128, 0x138, -1, -1, 0 },
    // Granite Ridge (LibreHardwareMonitor: PPT 1, package 11, SoC 54, fabric 71, uncore 75, memory 79,
    // per core temperature 333, clock 349)
    { 0x621202, 0x994, 0x2C, 0x04, -1, 0xD8, 0x11C, 0x12C, 0x13C, 0x534, 0x574, 16 },
};

// ---- state (sampling thread) ----------------------------------------------------------------------------

struct LpPrev { u64 aperf, mperf; f64 t; bool ok; };

static struct {
    bool zen5;
    f32 tdie_offset;            // k10temp per-part offset (Tdie = Tctl + offset)
    u32 ccd_base, ccd_count;
    bool ccd_valid_bit;
    bool svi2;
    u32 svi_p0, svi_p1;
    f64 energy_unit;            // J per count
    u32 pkg_prev, pp0_prev, pp1_prev;
    f64 energy_t;
    bool energy_ok, pp0_ok, pp1_ok;
    f32 mperf_mhz;              // rate MPERF counts at (P0 / max non-turbo)
    f32 tjmax;                  // Intel
    u32 lps;
    u8 lp_core[HW_MAX_LPS];     // physical core index of each logical processor (group 0)
    u32 cores;
    LpPrev prev[HW_MAX_LPS];
    u32 pm_version;
    const PmLayout* pm;
    bool pm_ok;
    alignas(8) f32 pm_table[1024];  // one 4 KiB page
} s;

static bool msr(u32 reg, u64* v) {
    u64 in = reg;
    return pawnio_call(&g_adv.cpu, "ioctl_read_msr", &in, 1, v, 1) == 0;
}

bool amd_smn_read(u32 addr, u32* value) {
    if (!g_adv.amd || !g_adv.cpu.h || !bus_lock(LOCK_PCI, 20)) return false;
    u64 in = addr, out = 0;
    const bool ok = pawnio_call(&g_adv.cpu, "ioctl_read_smn", &in, 1, &out, 1) == 0;
    bus_unlock(LOCK_PCI);
    *value = (u32)out;
    return ok;
}

static void map_cores() {
    s.lps = GetActiveProcessorCount(0);
    if (s.lps > HW_MAX_LPS) s.lps = HW_MAX_LPS;
    for (u32 i = 0; i < HW_MAX_LPS; ++i) s.lp_core[i] = (u8)i;
    s.cores = s.lps;
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    alignas(8) static u8 buf[64 * 1024];
    if (!size || size > sizeof(buf)) return;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buf;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &size)) return;
    u32 core = 0;
    for (u8* p = buf; p < buf + size && core < ADV_MAX_CORES; p += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)p)->Size) {
        const PROCESSOR_RELATIONSHIP* r = &((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)p)->Processor;
        if (r->GroupCount < 1 || r->GroupMask[0].Group != 0) continue;
        for (u32 lp = 0; lp < s.lps; ++lp)
            if (r->GroupMask[0].Mask & (1ull << lp)) s.lp_core[lp] = (u8)core;
        ++core;
    }
    if (core) s.cores = core;
}

// ---- AMD ------------------------------------------------------------------------------------------------

static f32 amd_cof_mhz(u64 v) {
    if (s.zen5) return (f32)(v & 0xFFF) * 5.0f;
    const u32 fid = (u32)(v & 0xFF), did = (u32)((v >> 8) & 0x3F);
    return did ? 200.0f * fid / did : NAN;
}

// k10temp: these parts report Tctl with a fixed offset over the real die temperature.
static f32 tdie_offset(const char* brand) {
    if (strstr(brand, "1600X") || strstr(brand, "1700X") || strstr(brand, "1800X")) return -20.0f;
    if (strstr(brand, "Threadripper 19") || strstr(brand, "Threadripper 29")) return -27.0f;
    if (strstr(brand, "2700X")) return -10.0f;
    return 0.0f;
}

static void amd_setup() {
    const u32 f = g_adv.family, m = g_adv.model;
    s.zen5 = f == 0x1A;
    s.tdie_offset = tdie_offset(g_sys.cpu.brand);
    // Per-CCD temperatures (LibreHardwareMonitor Amd17Cpu.cs; monolithic APUs have none).
    if ((f == 0x17 && (m == 0x31 || m == 0x71)) || (f == 0x19 && m == 0x21)) { s.ccd_base = SMN_CCD_ZEN2; s.ccd_count = 8; }
    else if ((f == 0x19 && m == 0x61) || (f == 0x1A && m == 0x44)) { s.ccd_base = SMN_CCD_ZEN4; s.ccd_count = 8; }
    else if (f == 0x1A && m <= 0x2F) { s.ccd_base = SMN_CCD_TURIN; s.ccd_count = 16; s.ccd_valid_bit = true; }
    // SVI2 telemetry exists on Zen 1-3; Zen 4 and later use SVI3, whose telemetry we don't decode (the PM table
    // carries the voltages on mapped versions instead).
    s.svi2 = f == 0x17 || (f == 0x19 && m < 0x60);
    if (m == 0x31 && f == 0x17) { s.svi_p0 = SMN_SVI + 0x14; s.svi_p1 = SMN_SVI + 0x10; }
    else if ((f == 0x17 && m == 0x71) || (f == 0x19 && m == 0x21)) { s.svi_p0 = SMN_SVI + 0x10; s.svi_p1 = SMN_SVI + 0xC; }
    else { s.svi_p0 = SMN_SVI + 0xC; s.svi_p1 = SMN_SVI + 0x10; }

    u64 v = 0;
    if (msr(MSR_AMD_PWR_UNIT, &v)) {
        s.energy_unit = ldexp(1.0, -(int)((v >> 8) & 0x1F));
        u64 e = 0;
        s.energy_ok = msr(MSR_AMD_PKG_ENERGY, &e);
        s.pkg_prev = (u32)e;
        s.energy_t = timer_now();
    }
    if (msr(MSR_AMD_PSTATE_0, &v) && (v >> 63)) s.mperf_mhz = amd_cof_mhz(v);
}

static f32 pm_value(i16 off) {
    if (off < 0 || !s.pm_ok || (u32)off / 4 >= ARRAY_COUNT(s.pm_table)) return NAN;
    const f32 v = s.pm_table[off / 4];
    return isfinite(v) ? v : NAN;
}

// Copies the PM table to DRAM and reads it (RyzenSMU ioctl_update_pm_table + ioctl_read_pm_table). The first
// transfer after boot can come back zeroed, so a zero first value gets one retry (LibreHardwareMonitor does too).
static bool pm_read(u32 bytes) {
    if (!g_adv.smu.h || !s.pm_version) return false;
    const u32 cells = (bytes + 7) / 8 > 512 ? 512 : (bytes + 7) / 8;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        if (!bus_lock(LOCK_PCI, 20)) return false;
        const bool ok = pawnio_call(&g_adv.smu, "ioctl_update_pm_table", nullptr, 0, nullptr, 0) == 0 &&
                        pawnio_call(&g_adv.smu, "ioctl_read_pm_table", nullptr, 0, (u64*)s.pm_table, cells) == 0;
        bus_unlock(LOCK_PCI);
        if (!ok) return false;
        if (s.pm_table[0] != 0.0f) return true;
    }
    return true;
}

static void pm_setup(AdvInfo* info) {
    u64 out[2] = {};
    if (!g_adv.smu.h || !bus_lock(LOCK_PCI, 50)) return;
    const bool ok = pawnio_call(&g_adv.smu, "ioctl_resolve_pm_table", nullptr, 0, out, 2) == 0;
    bus_unlock(LOCK_PCI);
    if (!ok) return;
    s.pm_version = (u32)out[0];
    for (u32 i = 0; i < ARRAY_COUNT(s_pm_layouts) && !s.pm; ++i)
        if (s_pm_layouts[i].version == s.pm_version) s.pm = &s_pm_layouts[i];
    info->pm_table_version = s.pm_version;
    info->pm_table_mapped = s.pm != nullptr;
}

static void amd_sample(AdvCpu* o) {
    u32 raw = 0;
    if (amd_smn_read(SMN_THM_TCON_CUR_TMP, &raw) && raw != 0xFFFFFFFFu) {
        f32 t = (f32)(raw >> 21) * 0.125f;
        if ((raw & 0x80000) || (raw & 0x30000) == 0x30000) t -= 49.0f;
        if (t > -40.0f && t < 150.0f) {
            o->tctl_c = t;
            o->temp_c = t + s.tdie_offset;
        }
    }
    for (u32 i = 0; i < s.ccd_count; ++i) {
        u32 v = 0;
        if (!amd_smn_read(s.ccd_base + i * 4, &v)) break;
        const bool present = s.ccd_valid_bit ? (v & 0x800) != 0 : (v & 0xFFF) != 0;
        const f32 t = (f32)(v & 0xFFF) * 0.125f - 305.0f;
        if (present && t > -40.0f && t < 125.0f) o->ccd_c[o->ccd_count++] = t;
    }
    if (s.svi2) {
        u32 tfn = 0, p0 = 0, p1 = 0;
        if (amd_smn_read(SMN_SVI + 0x8, &tfn) && amd_smn_read(s.svi_p0, &p0) && amd_smn_read(s.svi_p1, &p1)) {
            if (!(tfn & 1)) o->vcore_v = 1.55f - 0.00625f * (f32)((p0 >> 16) & 0xFF);
            const u32 m = g_adv.model;
            if (m == 0x11 || m == 0x21 || m == 0x71 || m == 0x31 || !(tfn & 2)) o->vsoc_v = 1.55f - 0.00625f * (f32)((p1 >> 16) & 0xFF);
        }
    }
    if (s.energy_ok) {
        u64 e = 0;
        const f64 now = timer_now();
        if (msr(MSR_AMD_PKG_ENERGY, &e)) {
            const u32 d = (u32)e - s.pkg_prev;      // 32-bit wrap
            const f64 dt = now - s.energy_t;
            if (dt > 0.05) o->package_w = (f32)(d * s.energy_unit / dt);
            s.pkg_prev = (u32)e;
            s.energy_t = now;
        }
    }
    // PM table: at most once per call, and the thread runs at 1 Hz.
    if (s.pm && pm_read(s.pm->size)) {
        s.pm_ok = true;
        const PmLayout* l = s.pm;
        const f32 tctl = pm_value(l->tctl);
        if (isnan(o->tctl_c) && tctl > 0.0f && tctl < 150.0f) o->tctl_c = o->temp_c = tctl;
        o->ppt_w = pm_value(l->ppt);
        if (isnan(o->vcore_v)) o->vcore_v = pm_value(l->vcore);
        if (isnan(o->vsoc_v)) o->vsoc_v = pm_value(l->vsoc);
        o->fclk_mhz = pm_value(l->fclk);
        o->uclk_mhz = pm_value(l->uclk);
        o->mclk_mhz = pm_value(l->mclk);
        const u32 n = l->cores < s.cores ? l->cores : s.cores;
        for (u32 c = 0; c < n && c < ADV_MAX_CORES; ++c) {
            const f32 t = l->core_temp >= 0 ? pm_value((i16)(l->core_temp + 4 * c)) : NAN;
            f32 f = l->core_clk >= 0 ? pm_value((i16)(l->core_clk + 4 * c)) : NAN;
            if (f > 0.0f && f < 20.0f) f *= 1000.0f;        // GHz -> MHz
            o->core_c[c] = t > 0.0f && t < 125.0f ? t : NAN;
            o->core_mhz[c] = f > 100.0f && f < 10000.0f ? f : NAN;
        }
        if (n) o->core_count = n;
    }
}

// ---- Intel ----------------------------------------------------------------------------------------------

static void intel_setup() {
    u64 v = 0;
    s.tjmax = msr(MSR_IA32_TEMPERATURE_TARGET, &v) && ((v >> 16) & 0xFF) ? (f32)((v >> 16) & 0xFF) : 100.0f;
    if (msr(MSR_PLATFORM_INFO, &v) && ((v >> 8) & 0xFF)) s.mperf_mhz = (f32)((v >> 8) & 0xFF) * 100.0f;
    if (msr(MSR_RAPL_POWER_UNIT, &v)) {
        s.energy_unit = ldexp(1.0, -(int)((v >> 8) & 0x1F));
        u64 e = 0;
        s.energy_ok = msr(MSR_PKG_ENERGY, &e);
        s.pkg_prev = (u32)e;
        s.pp0_ok = msr(MSR_PP0_ENERGY, &e);
        s.pp0_prev = (u32)e;
        s.pp1_ok = msr(MSR_PP1_ENERGY, &e);
        s.pp1_prev = (u32)e;
        s.energy_t = timer_now();
    }
}

static u32 intel_limits(u64 v, bool core) {
    u32 l = 0;
    if (v & (1u << 0)) l |= ADV_LIM_THERMAL;
    if (v & (1u << 2)) l |= ADV_LIM_PROCHOT;
    if (v & (1u << 4)) l |= ADV_LIM_CRITICAL;
    if (v & (1u << 10)) l |= ADV_LIM_POWER;
    if (core && (v & (1u << 12))) l |= ADV_LIM_CURRENT;
    if (core && (v & (1u << 14))) l |= ADV_LIM_CROSS;
    return l;
}

static void intel_sample(AdvCpu* o) {
    u64 v = 0;
    if (msr(MSR_IA32_PKG_THERM_STATUS, &v)) {
        o->temp_c = s.tjmax - (f32)((v >> 16) & 0x7F);
        o->limits |= intel_limits(v, false);
    }
    if (!s.energy_ok) return;
    const f64 now = timer_now(), dt = now - s.energy_t;
    u64 e = 0;
    if (msr(MSR_PKG_ENERGY, &e)) {
        if (dt > 0.05) o->package_w = (f32)((u32)((u32)e - s.pkg_prev) * s.energy_unit / dt);
        s.pkg_prev = (u32)e;
    }
    if (s.pp0_ok && msr(MSR_PP0_ENERGY, &e)) {
        if (dt > 0.05) o->cores_w = (f32)((u32)((u32)e - s.pp0_prev) * s.energy_unit / dt);
        s.pp0_prev = (u32)e;
    }
    if (s.pp1_ok && msr(MSR_PP1_ENERGY, &e)) {
        if (dt > 0.05) o->gfx_w = (f32)((u32)((u32)e - s.pp1_prev) * s.energy_unit / dt);
        s.pp1_prev = (u32)e;
    }
    s.energy_t = now;
}

// ---- per logical processor ------------------------------------------------------------------------------
// MSRs are per core: the thread visits each logical processor (affinity) and reads there. Skipped while a
// benchmark runs, so measured cores aren't interrupted 16 times a second.

static void sample_lps(AdvCpu* o) {
    if (isnan(s.mperf_mhz) || s.mperf_mhz <= 0.0f) return;
    const u32 ra = g_adv.amd ? MSR_AMD_APERF : MSR_IA32_APERF, rm = g_adv.amd ? MSR_AMD_MPERF : MSR_IA32_MPERF;
    HANDLE th = GetCurrentThread();
    const DWORD_PTR old = SetThreadAffinityMask(th, 1);
    if (!old) return;
    f32 worst = NAN;
    for (u32 lp = 0; lp < s.lps; ++lp) {
        o->lp_mhz[lp] = o->lp_busy[lp] = NAN;
        if (!SetThreadAffinityMask(th, (DWORD_PTR)1 << lp)) continue;
        u64 a = 0, m = 0, hw = 0, therm = 0;
        if (!msr(ra, &a) || !msr(rm, &m)) { s.prev[lp].ok = false; continue; }
        const f64 now = timer_now();
        const bool req_ok = g_adv.amd && msr(MSR_AMD_HW_PSTATE, &hw);
        if (g_adv.intel && msr(MSR_IA32_THERM_STATUS, &therm)) {
            const u32 c = s.lp_core[lp];
            if ((therm >> 31) & 1 && c < ADV_MAX_CORES) {
                const f32 t = s.tjmax - (f32)((therm >> 16) & 0x7F);
                if (isnan(o->core_c[c]) || t > o->core_c[c]) o->core_c[c] = t;
                if (c + 1 > o->core_count) o->core_count = c + 1;
            }
            o->limits |= intel_limits(therm, true);
        }
        LpPrev* p = &s.prev[lp];
        if (p->ok && m > p->mperf && a >= p->aperf && now > p->t) {
            const f64 da = (f64)(a - p->aperf), dm = (f64)(m - p->mperf), dt = now - p->t;
            const f32 mhz = (f32)(da / dm * s.mperf_mhz);
            const f32 busy = (f32)(dm / (dt * s.mperf_mhz * 1e6) * 100.0);
            if (mhz > 50.0f && mhz < 10000.0f) o->lp_mhz[lp] = mhz;
            o->lp_busy[lp] = busy > 100.0f ? 100.0f : busy;
            // Clock stretching: a busy core running slower than the P-state it was given.
            const f32 req = req_ok ? amd_cof_mhz(hw) : NAN;
            if (busy >= 90.0f && req > 0.0f && !isnan(o->lp_mhz[lp])) {
                const f32 st = (req - mhz) / req * 100.0f;
                if (isnan(worst) || st > worst) worst = st;
            }
        }
        *p = { a, m, now, true };
    }
    SetThreadAffinityMask(th, old);
    o->lp_count = s.lps;
    o->stretch_pct = worst;
}

// ---- entry points ---------------------------------------------------------------------------------------

void advcpu_setup(AdvInfo* info) {
    memset(&s, 0, sizeof(s));
    s.mperf_mhz = NAN;
    map_cores();
    if (!g_adv.cpu.h) return;
    if (g_adv.amd) {
        amd_setup();
        pm_setup(info);
    } else {
        intel_setup();
    }
}

void advcpu_close() { memset(&s, 0, sizeof(s)); }

void advcpu_sample(AdvCpu* o, bool hop) {
    memset(o, 0, sizeof(*o));
    o->temp_c = o->tctl_c = o->stretch_pct = o->package_w = o->cores_w = o->gfx_w = NAN;
    o->vcore_v = o->vsoc_v = o->ppt_w = o->fclk_mhz = o->uclk_mhz = o->mclk_mhz = NAN;
    for (u32 i = 0; i < ADV_MAX_CORES; ++i) o->core_c[i] = o->core_mhz[i] = NAN;
    if (!g_adv.cpu.h) return;
    if (g_adv.amd) amd_sample(o);
    else intel_sample(o);
    if (hop) sample_lps(o);
    else for (u32 i = 0; i < HW_MAX_LPS; ++i) s.prev[i].ok = false;     // stale after a gap
    o->time = timer_now();
}

u32 advcpu_pm_raw(f32* out, u32 max) {
    if (!s.pm_version || !pm_read(4096)) return 0;
    const u32 n = max < 1024 ? max : 1024;
    memcpy(out, s.pm_table, n * sizeof(f32));
    return n;
}
