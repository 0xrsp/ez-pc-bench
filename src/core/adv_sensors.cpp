// Advanced sensors through PawnIO (see adv_sensors.h): detection, the on / off switch, module handles, the
// one sampling thread and the snapshots consumers copy. The per-area readers are adv_cpu.cpp, adv_smbus.cpp
// and adv_umc.cpp. UNTESTED against the real driver (PawnIO wasn't installed on the development machine).
#include "core/adv_internal.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

AdvCtx g_adv;

static SRWLOCK s_ctl = SRWLOCK_INIT;    // init, enable, start / stop
static SRWLOCK s_snap = SRWLOCK_INIT;   // s_cpu / s_mem
static bool s_inited, s_enabled = true;
static AdvInfo s_info;
static AdvCpu s_cpu;
static AdvMem s_mem;
static u32 s_users;
static HANDLE s_thread, s_stop;

constexpr u32 TICK_MS = 1000;           // also the PM table rate: AMD asks for <= 1 Hz SMU polling
constexpr u32 DIMM_EVERY = 2;           // DIMM sensors every 2nd tick

const char* adv_status_text(AdvStatus st) {
    switch (st) {
        case ADV_NOT_INSTALLED: return "not installed";
        case ADV_NOT_RUNNING: return "installed, driver not running";
        case ADV_OFF: return "off";
        case ADV_NEEDS_ADMIN: return "needs administrator rights";
        case ADV_NO_MODULES: return "module files missing";
        case ADV_UNSUPPORTED: return "not supported on this PC";
        case ADV_ACTIVE: return "on";
    }
    return "?";
}

// ---- modules --------------------------------------------------------------------------------------------

static void add_module(PawnModuleId id) {
    const size_t len = strlen(s_info.modules);
    snprintf(s_info.modules + len, sizeof(s_info.modules) - len, "%s%s", len ? ", " : "", pawnio_module_name(id));
}

// Returns 0 when loaded; records the name.
static u32 open_module(Pawn* p, PawnModuleId id) {
    const u32 err = pawnio_open(p, id);
    if (!err) add_module(id);
    return err;
}

static void close_modules() {
    pawnio_close(&g_adv.cpu);
    pawnio_close(&g_adv.smu);
    pawnio_close(&g_adv.smbus);
    advcpu_close();
    advmem_close();
    s_info.modules[0] = 0;
}

static void open_modules() {
    const CpuInfo* c = &g_sys.cpu;
    g_adv.amd = !strcmp(c->vendor, "AuthenticAMD") && c->family >= 0x17 && c->family <= 0x1A;
    g_adv.intel = !strcmp(c->vendor, "GenuineIntel");
    g_adv.family = c->family;
    g_adv.model = c->model;
    if (!g_adv.amd && !g_adv.intel) {
        s_info.status = ADV_UNSUPPORTED;
        snprintf(s_info.detail, sizeof(s_info.detail), "No decoder for this CPU (%s, family 0x%X).", c->vendor, c->family);
        return;
    }
    const u32 cpu_err = open_module(&g_adv.cpu, g_adv.amd ? PM_AMD17 : PM_INTEL_MSR);
    const u32 smu_err = g_adv.amd ? open_module(&g_adv.smu, PM_RYZEN_SMU) : 0;
    const PawnModuleId bus = g_adv.amd ? PM_SMBUS_PIIX4 : PM_SMBUS_I801;
    const u32 bus_err = open_module(&g_adv.smbus, bus);
    if (!g_adv.cpu.h && !g_adv.smu.h && !g_adv.smbus.h) {
        const bool missing = cpu_err == ERROR_FILE_NOT_FOUND && bus_err == ERROR_FILE_NOT_FOUND;
        s_info.status = missing ? ADV_NO_MODULES : ADV_UNSUPPORTED;
        if (missing)
            snprintf(s_info.detail, sizeof(s_info.detail), "PawnIO is installed, but this build carries no signed PawnIO "
                     "modules: put %s.bin and %s.bin into a \"pawnio\" folder next to the exe.",
                     pawnio_module_name(g_adv.amd ? PM_AMD17 : PM_INTEL_MSR), pawnio_module_name(bus));
        else
            snprintf(s_info.detail, sizeof(s_info.detail), "The PawnIO modules refused this PC (errors %u / %u).", cpu_err, bus_err);
        return;
    }
    s_info.status = ADV_ACTIVE;
    if (cpu_err)
        snprintf(s_info.detail, sizeof(s_info.detail), "CPU module %s not loaded (error %u).",
                 pawnio_module_name(g_adv.amd ? PM_AMD17 : PM_INTEL_MSR), cpu_err);
    else if (smu_err)
        snprintf(s_info.detail, sizeof(s_info.detail), "RyzenSMU not loaded (error %u): no PM table values.", smu_err);
    if (bus_err)
        snprintf(s_info.smbus_note, sizeof(s_info.smbus_note), "%s not loaded (error %u): SMBus controller missing, "
                 "disabled or at a non-default address; no DIMM sensors.", pawnio_module_name(bus), bus_err);

    advcpu_setup(&s_info);
    AdvCpu first;
    advcpu_sample(&first, true);        // primes the energy / APERF counters, PM table clocks for the timings
    if (!g_memdetail.read) {
        g_memdetail.read = true;
        advmem_setup(&g_memdetail);
        advumc_read(&g_memdetail.timings, &first);
    }
    if (!s_info.smbus_note[0] && g_memdetail.spd_note[0])
        snprintf(s_info.smbus_note, sizeof(s_info.smbus_note), "%s", g_memdetail.spd_note);
}

static void detect_and_open() {
    char version[24];
    const PawnDetect d = pawnio_detect(version, sizeof(version));
    memset(&s_info, 0, sizeof(s_info));
    snprintf(s_info.driver_version, sizeof(s_info.driver_version), "%s", version);
    if (d == PAWN_ABSENT) {
        s_info.status = ADV_NOT_INSTALLED;
        snprintf(s_info.detail, sizeof(s_info.detail), "PawnIO isn't installed. Its official installer is at %s; "
                 "EzPcBench never installs drivers itself.", ADV_PAWNIO_URL);
    } else if (!s_enabled) {
        s_info.status = ADV_OFF;
        snprintf(s_info.detail, sizeof(s_info.detail), "PawnIO is installed; advanced sensors are switched off.");
    } else if (d == PAWN_NOT_RUNNING) {
        s_info.status = ADV_NOT_RUNNING;
        snprintf(s_info.detail, sizeof(s_info.detail), "PawnIO %s is installed but its device isn't available (a restart "
                 "may be pending).", version);
    } else if (d == PAWN_NO_ACCESS) {
        s_info.status = ADV_NEEDS_ADMIN;
        snprintf(s_info.detail, sizeof(s_info.detail), "PawnIO only opens for administrators: run EzPcBench as administrator.");
    } else {
        open_modules();
    }
}

// ---- sampling thread ------------------------------------------------------------------------------------

static DWORD WINAPI adv_thread(void*) {
    u32 tick = 0;
    do {
        // While a benchmark runs: no per-core hopping and no SMBus traffic (see adv_sensors.h).
        const bool quiet = runner_is_running();
        AdvCpu c;
        advcpu_sample(&c, !quiet);
        AdvMem m;
        const bool mem = !quiet && tick % DIMM_EVERY == 0;
        if (mem) advmem_sample(&m);
        AcquireSRWLockExclusive(&s_snap);
        s_cpu = c;
        if (mem) s_mem = m;
        ReleaseSRWLockExclusive(&s_snap);
        ++tick;
    } while (WaitForSingleObject(s_stop, TICK_MS) == WAIT_TIMEOUT);
    return 0;
}

static void start_thread() {
    if (s_thread || s_info.status != ADV_ACTIVE) return;
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_thread = CreateThread(nullptr, 0, adv_thread, nullptr, 0, nullptr);
    // Short bursts once a second that must not starve behind HIGHEST-priority benchmark threads (as the run monitor).
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

static void stop_thread() {
    if (!s_thread) return;
    SetEvent(s_stop);
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    CloseHandle(s_stop);
    s_thread = s_stop = nullptr;
    AcquireSRWLockExclusive(&s_snap);
    memset(&s_cpu, 0, sizeof(s_cpu));
    memset(&s_mem, 0, sizeof(s_mem));
    ReleaseSRWLockExclusive(&s_snap);
}

// ---- API ------------------------------------------------------------------------------------------------

static void init_locked() {
    if (s_inited) return;
    s_inited = true;
    detect_and_open();
}

void adv_init() {
    AcquireSRWLockExclusive(&s_ctl);
    init_locked();
    ReleaseSRWLockExclusive(&s_ctl);
}

void adv_shutdown() {
    AcquireSRWLockExclusive(&s_ctl);
    stop_thread();
    close_modules();
    ReleaseSRWLockExclusive(&s_ctl);
}

void adv_set_enabled(bool on) {
    AcquireSRWLockExclusive(&s_ctl);
    const bool changed = s_enabled != on;
    s_enabled = on;
    if (s_inited && changed) {
        stop_thread();
        close_modules();
        detect_and_open();
        if (s_users) start_thread();
    }
    ReleaseSRWLockExclusive(&s_ctl);
}

bool adv_enabled() { return s_enabled; }

AdvStatus adv_status(AdvInfo* info) {
    AcquireSRWLockShared(&s_ctl);
    const AdvStatus st = s_info.status;
    if (info) *info = s_info;
    ReleaseSRWLockShared(&s_ctl);
    return st;
}

void adv_start() {
    AcquireSRWLockExclusive(&s_ctl);
    init_locked();
    if (s_users++ == 0) start_thread();
    ReleaseSRWLockExclusive(&s_ctl);
}

void adv_stop() {
    AcquireSRWLockExclusive(&s_ctl);
    if (s_users && --s_users == 0) stop_thread();
    ReleaseSRWLockExclusive(&s_ctl);
}

bool adv_cpu_snapshot(AdvCpu* out) {
    AcquireSRWLockShared(&s_snap);
    *out = s_cpu;
    ReleaseSRWLockShared(&s_snap);
    return out->time > 0.0;
}

bool adv_mem_snapshot(AdvMem* out) {
    AcquireSRWLockShared(&s_snap);
    *out = s_mem;
    ReleaseSRWLockShared(&s_snap);
    return out->time > 0.0;
}

f32 adv_cpu_temp(f32 max_age_s) {
    AcquireSRWLockShared(&s_snap);
    const f32 t = s_cpu.time > 0.0 && timer_now() - s_cpu.time <= max_age_s ? s_cpu.temp_c : NAN;
    ReleaseSRWLockShared(&s_snap);
    return t;
}

// ---- CLI report ------------------------------------------------------------------------------------------

static void print_val(const char* label, f32 v, const char* unit, const char* fmt = "%.1f") {
    if (isnan(v)) return;
    printf("  %-22s ", label);
    printf(fmt, v);
    printf(" %s\n", unit);
}

void adv_print_report() {
    adv_init();
    AcquireSRWLockExclusive(&s_ctl);
    printf("Advanced sensors (PawnIO): %s\n", adv_status_text(s_info.status));
    if (s_info.driver_version[0]) printf("  driver                 %s\n", s_info.driver_version);
    if (s_info.modules[0]) printf("  modules                %s\n", s_info.modules);
    if (s_info.detail[0]) printf("  %s\n", s_info.detail);
    if (s_info.smbus_note[0]) printf("  SMBus: %s\n", s_info.smbus_note);
    if (s_info.status != ADV_ACTIVE || s_thread) {
        ReleaseSRWLockExclusive(&s_ctl);
        return;
    }
    AdvCpu c;
    AdvMem m;
    advcpu_sample(&c, true);
    Sleep(TICK_MS);
    advcpu_sample(&c, true);
    advmem_sample(&m);
    printf("\nCPU\n");
    print_val("temperature", c.temp_c, "\xC2\xB0""C");
    if (c.tctl_c != c.temp_c) print_val("Tctl", c.tctl_c, "\xC2\xB0""C");
    for (u32 i = 0; i < c.ccd_count; ++i) printf("  CCD %-18u %.1f \xC2\xB0""C\n", i + 1, c.ccd_c[i]);
    for (u32 i = 0; i < c.core_count; ++i)
        if (!isnan(c.core_c[i]) || !isnan(c.core_mhz[i])) printf("  core %-17u %.1f \xC2\xB0""C  %.0f MHz\n", i, c.core_c[i], c.core_mhz[i]);
    print_val("package power", c.package_w, "W");
    print_val("cores power", c.cores_w, "W");
    print_val("graphics power", c.gfx_w, "W");
    print_val("SMU power (PPT)", c.ppt_w, "W");
    print_val("Vcore", c.vcore_v, "V", "%.3f");
    print_val("VSoC", c.vsoc_v, "V", "%.3f");
    print_val("FCLK", c.fclk_mhz, "MHz", "%.0f");
    print_val("UCLK", c.uclk_mhz, "MHz", "%.0f");
    print_val("MCLK", c.mclk_mhz, "MHz", "%.0f");
    print_val("clock stretching", c.stretch_pct, "%");
    if (c.limits) printf("  limits                 0x%X\n", c.limits);
    for (u32 i = 0; i < c.lp_count; ++i)
        if (!isnan(c.lp_mhz[i])) printf("  LP %-19u %.0f MHz, %.0f %% busy\n", i, c.lp_mhz[i], c.lp_busy[i]);
    if (m.count) printf("\nDIMM sensors\n");
    for (u32 i = 0; i < m.count; ++i)
        printf("  0x%02X %-17s %.2f \xC2\xB0""C\n", m.dimms[i].addr, m.dimms[i].ddr5 ? "(SPD5118)" : "(TSE2004)", m.dimms[i].temp_c);
    char line[192];
    if (memdetail_timings_line(line, sizeof(line))) printf("\nTimings  %s\n", line);
    else if (g_memdetail.timings.note[0]) printf("\nTimings  %s\n", g_memdetail.timings.note);
    char lines[8][192];
    for (u32 i = 0, n = memdetail_spd_lines(lines, 8); i < n; ++i) printf("%s\n", lines[i]);
    if (g_memdetail.spd_note[0]) printf("SPD      %s\n", g_memdetail.spd_note);
    if (s_info.pm_table_version) {
        static f32 pm[1024];
        const u32 n = advcpu_pm_raw(pm, 1024);
        printf("\nPM table version 0x%06X (%s), raw dump: index  byte offset  value\n", s_info.pm_table_version,
               s_info.pm_table_mapped ? "layout known" : "layout not mapped yet");
        for (u32 i = 0; i < n; ++i) printf("  %4u  0x%03X  %g\n", i, i * 4, pm[i]);
    }
    ReleaseSRWLockExclusive(&s_ctl);
}
