// Stability tester controller (see stability.h): runs the selected tests in sequence or together on its own
// threads, samples sensors once a second and enforces the temperature limits, counts WHEA events, keeps the
// first errors, and writes the report.
#include "stability/stab_internal.h"
#include "core/hwread.h"
#include "core/physdisk.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/timer.h"
#include "core/toon.h"
#include "core/whea.h"
#include <windows.h>
#include <pdh.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

StabRun g_stab;

const StabTestDesc g_stab_tests[STAB_COUNT] = {
    { "cpu", "CPU", "G ops checked",
      "Self-checking kernels on every logical processor: integer hashing, SSE multiply-add, AVX2 and AVX-512 FMA "
      "on cache-resident data (most heat), and FMA over a large buffer streamed from RAM. Every block's result is "
      "compared with a reference computed by a different instruction path; errors are attributed to the logical "
      "processor that made them." },
    { "cache", "Cache", "GB checked",
      "Writes, reads back and inverts patterns in L1-, L2- and L3-sized buffers on every logical processor." },
    { "cores", "Per-core cycling", "G ops checked",
      "One physical core at a time under a heavy single-thread load (the CPU kernels), moving to the next core "
      "after the per-core time, like CoreCycler. Finds cores that fail with Curve Optimizer / undervolt settings "
      "that all-core loads hide (single-core boost runs at higher clocks and lower voltage margins)." },
    { "mem", "Memory", "GB checked",
      "Fills locked, pre-faulted RAM with patterns (address in address, walking 1s / 0s, random, moving "
      "inversions) on all threads and reads them back. Runs inside Windows, so it can only test the memory Windows "
      "gives it; a boot-time tester like MemTest86 reaches all of it." },
    { "vram", "VRAM", "GB checked",
      "Allocates most of the GPU's free memory in chunks and pattern-tests each on the GPU (write, read back, "
      "compare), counting mismatches per chunk. Direct3D 12, selected GPU." },
    { "gpu", "GPU errors", "dispatches",
      "Compute and render workloads with known answers, checked on every iteration: a compute shader whose results "
      "are compared with references computed on the CPU, and a rendered image checked pixel by pixel. Catches "
      "unstable GPU overclocks / undervolts." },
    { "power", "GPU max power", "dispatches",
      "Maximum-power GPU load (dense FMA plus VRAM streaming). Only checks that the GPU keeps running. Draws as much "
      "power as the GPU allows: watch the temperatures; the run stops at the GPU temperature limit." },
    { "disk", "Disk verify", "GB checked",
      "Writes a temporary file with known data (unbuffered, write-through), reads it back and compares every byte. "
      "Catches bad cables, controllers and USB bridges. The file is deleted afterwards." },
};

const char* const g_cpu_kernel_ids[CK_COUNT] = { "mixed", "int", "sse", "avx2", "avx512", "large" };
const char* const g_cpu_kernel_names[CK_COUNT] = { "Mixed (rotate every 4 s)", "Integer", "SSE multiply-add",
                                                   "AVX2 FMA", "AVX-512 FMA", "Large data (streams RAM)" };

const char* stab_status_name(StabStatus s) {
    switch (s) {
        case SS_PENDING: return "pending";
        case SS_RUNNING: return "running";
        case SS_PASS: return "pass";
        case SS_FAIL: return "fail";
        case SS_STOPPED: return "stopped";
        case SS_ERROR: return "error";
        default: return "not run";
    }
}

static SRWLOCK s_lock = SRWLOCK_INIT;      // error log, stop reason, sensors
static StabError s_log[STAB_MAX_LOG];
static std::atomic<u32> s_log_count;
static StabSensors s_sens;
static HANDLE s_thread;

// ---- helpers for the tests ------------------------------------------------------------------------------

void stab_error(StabTest test, i32 lp, i32 chunk, u64 address, const char* what, u64 expected, u64 actual) {
    StabTestState* s = &g_stab.tests[test];
    s->errors.fetch_add(1, std::memory_order_relaxed);
    f64 none = -1.0;
    s->first_error_s.compare_exchange_strong(none, timer_now() - s->start_t, std::memory_order_relaxed);
    if (lp >= 0 && lp < (i32)STAB_MAX_LP) s->lp_errors[lp].fetch_add(1, std::memory_order_relaxed);
    if (chunk >= 0 && chunk < (i32)STAB_MAX_CHUNKS) s->chunk_errors[chunk].fetch_add(1, std::memory_order_relaxed);
    if (s_log_count.load(std::memory_order_relaxed) < STAB_MAX_LOG) {
        AcquireSRWLockExclusive(&s_lock);
        const u32 n = s_log_count.load(std::memory_order_relaxed);
        if (n < STAB_MAX_LOG) {
            StabError* e = &s_log[n];
            e->test = test;
            e->t = timer_now() - g_stab.start_t;
            e->lp = lp;
            e->chunk = chunk;
            e->address = address;
            snprintf(e->what, sizeof(e->what), "%s", what);
            e->expected = expected;
            e->actual = actual;
            s_log_count.store(n + 1, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&s_lock);
    }
    if (g_stab.cfg.stop_on_error && !g_stab.stop.load(std::memory_order_relaxed)) {
        char why[96];
        snprintf(why, sizeof(why), "stopped at the first error (%s)", g_stab_tests[test].name);
        stab_stop(why);
    }
}

void stab_add_errors(StabTest test, i32 chunk, u64 n) {
    if (!n) return;
    StabTestState* s = &g_stab.tests[test];
    s->errors.fetch_add(n, std::memory_order_relaxed);
    f64 none = -1.0;
    s->first_error_s.compare_exchange_strong(none, timer_now() - s->start_t, std::memory_order_relaxed);
    if (chunk >= 0 && chunk < (i32)STAB_MAX_CHUNKS) s->chunk_errors[chunk].fetch_add((u32)n, std::memory_order_relaxed);
    if (g_stab.cfg.stop_on_error && !g_stab.stop.load(std::memory_order_relaxed)) {
        char why[96];
        snprintf(why, sizeof(why), "stopped at the first error (%s)", g_stab_tests[test].name);
        stab_stop(why);
    }
}

bool stab_done(f64 deadline) { return g_stab.stop.load(std::memory_order_relaxed) || timer_now() >= deadline; }

void stab_work_mt(StabTest t, f64 add) {
    std::atomic<f64>& w = g_stab.tests[t].work;
    f64 cur = w.load(std::memory_order_relaxed);
    while (!w.compare_exchange_weak(cur, cur + add, std::memory_order_relaxed)) {}
}

void stab_fail_setup(StabTest test, const char* fmt, ...) {
    StabTestState* s = &g_stab.tests[test];
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->note, sizeof(s->note), fmt, args);
    va_end(args);
    s->status.store(SS_ERROR, std::memory_order_release);
}

struct ThreadStart {
    StabThreadFn fn;
    void* user;
    u32 index, lp;
};

static DWORD WINAPI stab_thread_main(void* arg) {
    const ThreadStart* t = (const ThreadStart*)arg;
    pin_current_thread(t->lp);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    THREAD_POWER_THROTTLING_STATE tt = {};      // no EcoQoS: a background window must still get full clocks
    tt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    tt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tt, sizeof(tt));
    t->fn(t->index, t->user);
    return 0;
}

void stab_run_threads(u32 count, const u32* lps, StabThreadFn fn, void* user) {
    if (count > STAB_MAX_LP) count = STAB_MAX_LP;
    ThreadStart starts[STAB_MAX_LP];
    HANDLE threads[STAB_MAX_LP];
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        starts[i] = { fn, user, i, lps[i] };
        HANDLE h = CreateThread(nullptr, 1024 * 1024, stab_thread_main, &starts[i], 0, nullptr);
        if (h) threads[n++] = h;
    }
    if (n) WaitForMultipleObjects(n, threads, TRUE, INFINITE);
    for (u32 i = 0; i < n; ++i) CloseHandle(threads[i]);
}

// ---- monitor ----------------------------------------------------------------------------------------------
// Own readers (core/hwread.*) on a high-priority thread, so the limits work while every core is loaded. CPU
// temperature is the hottest ACPI thermal zone: the only driverless source, and on many laptops it lags the CPU or
// doesn't follow it at all (PLAN.md M7 notes), so the CPU limit is best effort.

struct Monitor {
    CpuReader cpu;
    GpuReader gpu;
    PDH_HQUERY zq;
    PDH_HCOUNTER zones;
    bool zones_precise;
    HANDLE disks[PHYSDISK_MAX];
    u32 disk_count;
    f64 last_t;
    f64 wsum[SH_COUNT], w[SH_COUNT];
};

static void monitor_open(Monitor* m) {
    memset(m, 0, sizeof(*m));
    cpu_reader_open(&m->cpu);
    gpu_reader_open(&m->gpu, g_stab.cfg.gpu_adapter);
    if (PdhOpenQueryW(nullptr, 0, &m->zq) == ERROR_SUCCESS) {
        m->zones_precise = PdhAddEnglishCounterW(m->zq, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0,
                                                 &m->zones) == ERROR_SUCCESS;
        if (!m->zones_precise &&
            PdhAddEnglishCounterW(m->zq, L"\\Thermal Zone Information(*)\\Temperature", 0, &m->zones) != ERROR_SUCCESS) {
            PdhCloseQuery(m->zq);
            m->zq = nullptr;
        } else {
            PdhCollectQueryData(m->zq);
        }
    }
    PhysDisk disks[PHYSDISK_MAX];
    const u32 n = physdisk_enum(disks, PHYSDISK_MAX);
    for (u32 i = 0; i < n; ++i) {
        if (disks[i].virtual_disk) continue;
        HANDLE h = physdisk_open(disks[i].number, false);
        if (h != INVALID_HANDLE_VALUE) m->disks[m->disk_count++] = h;
    }
}

static void monitor_close(Monitor* m) {
    cpu_reader_close(&m->cpu);
    gpu_reader_close(&m->gpu);
    if (m->zq) PdhCloseQuery(m->zq);
    for (u32 i = 0; i < m->disk_count; ++i) CloseHandle(m->disks[i]);
}

static f32 hottest_zone(Monitor* m) {
    f32 hot = NAN;
    if (!m->zq || PdhCollectQueryData(m->zq) != ERROR_SUCCESS) return hot;
    alignas(8) static u8 buf[16384];
    DWORD size = sizeof(buf), count = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf;
    if (PdhGetFormattedCounterArrayW(m->zones, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return hot;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        const f64 v = items[i].FmtValue.doubleValue;
        const f32 c = (f32)(m->zones_precise ? v / 10.0 - 273.15 : v - 273.15);
        if (c > -40.0f && c < 150.0f && !(c <= hot)) hot = c;
    }
    return hot;
}

static void monitor_sample(Monitor* m, f32 v[SH_COUNT]) {
    for (u32 i = 0; i < SH_COUNT; ++i) v[i] = NAN;
    f32 util[HW_MAX_LPS], mhz[HW_MAX_LPS];
    const u32 n = cpu_reader_sample(&m->cpu, util, mhz);
    if (n) {
        f64 sum = 0.0, wsum = 0.0, w = 0.0;
        for (u32 i = 0; i < n; ++i) {
            sum += util[i];
            if (util[i] >= 50.0f && !isnan(mhz[i])) { wsum += (f64)mhz[i] * util[i]; w += util[i]; }
        }
        v[SH_CPU_UTIL] = (f32)(sum / n);
        if (w > 0.0) v[SH_CPU_MHZ] = (f32)(wsum / w);
    }
    v[SH_CPU_TEMP] = hottest_zone(m);
    gpu_reader_collect(&m->gpu);
    GpuReading g;
    gpu_reader_read(&m->gpu, g_stab.cfg.gpu_adapter, &g, false);
    v[SH_GPU_MHZ] = g.core_mhz;
    v[SH_GPU_UTIL] = g.util;
    v[SH_GPU_TEMP] = g.temp_c;
    v[SH_GPU_POWER] = g.power_pct;
    for (u32 i = 0; i < m->disk_count; ++i) {
        f32 t = 0.0f, warn = 0.0f;
        if (physdisk_temperature(m->disks[i], &t, &warn) && !(t <= v[SH_DRIVE_TEMP])) v[SH_DRIVE_TEMP] = t;
    }
}

static void check_limits(const f32 v[SH_COUNT]) {
    const StabConfig* c = &g_stab.cfg;
    char why[160] = "";
    if (c->max_cpu_c > 0.0f && v[SH_CPU_TEMP] >= c->max_cpu_c)
        snprintf(why, sizeof(why), "CPU temperature %.0f \xC2\xB0" "C reached the %.0f \xC2\xB0" "C limit", v[SH_CPU_TEMP], c->max_cpu_c);
    else if (c->max_gpu_c > 0.0f && v[SH_GPU_TEMP] >= c->max_gpu_c)
        snprintf(why, sizeof(why), "GPU temperature %.0f \xC2\xB0" "C reached the %.0f \xC2\xB0" "C limit", v[SH_GPU_TEMP], c->max_gpu_c);
    else if (c->max_drive_c > 0.0f && v[SH_DRIVE_TEMP] >= c->max_drive_c)
        snprintf(why, sizeof(why), "drive temperature %.0f \xC2\xB0" "C reached the %.0f \xC2\xB0" "C limit", v[SH_DRIVE_TEMP], c->max_drive_c);
    SYSTEM_POWER_STATUS ps;
    if (!why[0] && GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0 && ps.BatteryLifePercent <= 10)
        snprintf(why, sizeof(why), "battery down to %u%%", ps.BatteryLifePercent);
    if (why[0] && !g_stab.stop.load()) {
        g_stab.limit_stop = true;
        stab_stop(why);
    }
}

static void monitor_push(Monitor* m, const f32 v[SH_COUNT]) {
    const f64 now = timer_now();
    const f64 dt = m->last_t > 0.0 ? now - m->last_t : 1.0;
    m->last_t = now;
    AcquireSRWLockExclusive(&s_lock);
    StabSensors* s = &s_sens;
    for (u32 i = 0; i < SH_COUNT; ++i) {
        s->now[i] = v[i];
        if (!isnan(v[i])) {
            m->wsum[i] += v[i] * dt;
            m->w[i] += dt;
            s->avg[i] = (f32)(m->wsum[i] / m->w[i]);
            if (!(v[i] <= s->max[i])) s->max[i] = v[i];
        }
        if (s->hist_count == STAB_HIST) memmove(s->hist[i], s->hist[i] + 1, (STAB_HIST - 1) * sizeof(f32));
        s->hist[i][s->hist_count == STAB_HIST ? STAB_HIST - 1 : s->hist_count] = v[i];
    }
    if (s->hist_count < STAB_HIST) s->hist_count++;
    ReleaseSRWLockExclusive(&s_lock);
}

static HANDLE s_mon_stop;
static WheaCounts s_whea0;

static void update_whea() {
    WheaCounts w;
    whea_snapshot(&w);
    g_stab.whea_active = w.active;
    g_stab.whea_corrected = w.corrected - s_whea0.corrected;
    g_stab.whea_uncorrected = w.uncorrected - s_whea0.uncorrected;
}


static DWORD WINAPI monitor_main(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Monitor* m = (Monitor*)VirtualAlloc(nullptr, sizeof(Monitor), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) return 0;
    monitor_open(m);
    do {
        f32 v[SH_COUNT];
        monitor_sample(m, v);
        monitor_push(m, v);
        check_limits(v);
        update_whea();
    } while (WaitForSingleObject(s_mon_stop, 1000) == WAIT_TIMEOUT);
    monitor_close(m);
    VirtualFree(m, 0, MEM_RELEASE);
    return 0;
}

// ---- controller -----------------------------------------------------------------------------------------

static void run_test(StabTest t, f64 deadline) {
    StabTestState* s = &g_stab.tests[t];
    if (g_stab.stop.load()) {           // stopped before this test's turn
        s->status.store(SS_NOT_RUN, std::memory_order_release);
        return;
    }
    s->start_t = timer_now();
    s->status.store(SS_RUNNING, std::memory_order_release);
    switch (t) {
        case STAB_CPU: stab_cpu_run(deadline); break;
        case STAB_CACHE: stab_cache_run(deadline); break;
        case STAB_CORES: stab_cores_run(deadline); break;
        case STAB_MEM: stab_mem_run(deadline); break;
        case STAB_DISK: stab_disk_run(deadline); break;
        default: stab_gpu_run(t, deadline); break;
    }
    s->end_t = timer_now();
    s->phase.store(nullptr, std::memory_order_relaxed);
    if (s->status.load() == SS_ERROR) return;
    StabStatus st = SS_PASS;
    if (s->errors.load()) st = SS_FAIL;
    else if (isfinite(deadline) && s->end_t < deadline - 0.5 && g_stab.stop.load()) st = SS_STOPPED;
    s->status.store(st, std::memory_order_release);
}

struct TogetherArg { StabTest test; f64 deadline; };

static DWORD WINAPI together_main(void* arg) {
    const TogetherArg* a = (const TogetherArg*)arg;
    run_test(a->test, a->deadline);
    return 0;
}

static DWORD WINAPI controller_main(void*) {
    StabRun* r = &g_stab;
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);    // no sleep mid-test
    whea_start();
    whea_snapshot(&s_whea0);
    s_mon_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE mon = CreateThread(nullptr, 0, monitor_main, nullptr, 0, nullptr);

    const f64 secs = r->cfg.seconds ? (f64)r->cfg.seconds : INFINITY;
    if (r->cfg.together) {
        TogetherArg args[STAB_COUNT];
        HANDLE threads[STAB_COUNT];
        u32 n = 0;
        const f64 deadline = r->start_t + secs;
        for (u32 t = 0; t < STAB_COUNT; ++t) {
            if (!r->cfg.run[t]) continue;
            args[n] = { (StabTest)t, deadline };
            if (HANDLE h = CreateThread(nullptr, 0, together_main, &args[n], 0, nullptr)) threads[n++] = h;
        }
        if (n) WaitForMultipleObjects(n, threads, TRUE, INFINITE);
        for (u32 i = 0; i < n; ++i) CloseHandle(threads[i]);
    } else {
        for (u32 t = 0; t < STAB_COUNT; ++t) {
            if (!r->cfg.run[t]) continue;
            r->current.store((i32)t);
            run_test((StabTest)t, timer_now() + secs);
            update_whea();
        }
        r->current.store(-1);
    }

    SetEvent(s_mon_stop);
    if (mon) {
        WaitForSingleObject(mon, INFINITE);
        CloseHandle(mon);
    }
    CloseHandle(s_mon_stop);
    update_whea();
    whea_stop();
    SetThreadExecutionState(ES_CONTINUOUS);
    r->end_t = timer_now();
    r->running.store(false, std::memory_order_release);
    return 0;
}

bool stab_start(const StabConfig* cfg, char* err, u32 err_size) {
    if (stab_is_running()) { snprintf(err, err_size, "a stability test is already running"); return false; }
    bool any = false;
    for (u32 t = 0; t < STAB_COUNT; ++t) any |= cfg->run[t];
    if (!any) { snprintf(err, err_size, "no test selected"); return false; }
    stab_wait();
    if (!g_topo.lp_count) topo_init();

    StabRun* r = &g_stab;
    r->cfg = *cfg;
    if (r->cfg.gpu_adapter >= g_sys.gpu_count) r->cfg.gpu_adapter = sysinfo_default_gpu();
    r->stop.store(false);
    r->current.store(-1);
    r->stop_reason[0] = 0;
    r->limit_stop = false;
    r->whea_corrected = r->whea_uncorrected = 0;
    r->whea_active = false;
    for (u32 t = 0; t < STAB_COUNT; ++t) {
        StabTestState* s = &r->tests[t];
        s->status.store(cfg->run[t] ? SS_PENDING : SS_NOT_RUN);
        s->errors.store(0);
        s->first_error_s.store(-1.0);
        s->work.store(0.0);
        s->phase.store(nullptr);
        for (u32 i = 0; i < STAB_MAX_LP; ++i) { s->lp_errors[i].store(0); s->lp_seconds[i].store(0.0f); }
        for (u32 i = 0; i < STAB_MAX_CHUNKS; ++i) s->chunk_errors[i].store(0);
        s->chunk_count = 0;
        s->chunk_bytes = s->size_bytes = 0;
        s->start_t = s->end_t = 0.0;
        s->note[0] = 0;
    }
    AcquireSRWLockExclusive(&s_lock);
    s_log_count.store(0);
    memset(&s_sens, 0, sizeof(s_sens));
    for (u32 i = 0; i < SH_COUNT; ++i) s_sens.now[i] = s_sens.avg[i] = s_sens.max[i] = NAN;
    ReleaseSRWLockExclusive(&s_lock);

    r->start_t = timer_now();
    r->end_t = 0.0;
    r->running.store(true, std::memory_order_release);
    s_thread = CreateThread(nullptr, 0, controller_main, nullptr, 0, nullptr);
    if (!s_thread) {
        r->running.store(false);
        snprintf(err, err_size, "cannot create the controller thread");
        return false;
    }
    return true;
}

void stab_stop(const char* reason) {
    AcquireSRWLockExclusive(&s_lock);
    if (!g_stab.stop.load() && g_stab.running.load()) snprintf(g_stab.stop_reason, sizeof(g_stab.stop_reason), "%s", reason);
    g_stab.stop.store(true);
    ReleaseSRWLockExclusive(&s_lock);
}

bool stab_is_running() { return g_stab.running.load(std::memory_order_acquire); }

void stab_wait() {
    if (!s_thread) return;
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    s_thread = nullptr;
}

f64 stab_elapsed() {
    if (g_stab.start_t <= 0.0) return 0.0;
    return (stab_is_running() || g_stab.end_t <= 0.0 ? timer_now() : g_stab.end_t) - g_stab.start_t;
}

u64 stab_total_errors() {
    u64 n = 0;
    for (u32 t = 0; t < STAB_COUNT; ++t) n += g_stab.tests[t].errors.load(std::memory_order_relaxed);
    return n;
}

u32 stab_error_log(StabError* out, u32 max) {
    AcquireSRWLockShared(&s_lock);
    u32 n = s_log_count.load();
    if (n > max) n = max;
    memcpy(out, s_log, n * sizeof(StabError));
    ReleaseSRWLockShared(&s_lock);
    return n;
}

void stab_sensors(StabSensors* out) {
    AcquireSRWLockShared(&s_lock);
    *out = s_sens;
    ReleaseSRWLockShared(&s_lock);
}

const char* stab_verdict(char* why, u32 size) {
    const StabRun* r = &g_stab;
    char failed[160] = "", errored[160] = "";
    for (u32 t = 0; t < STAB_COUNT; ++t) {
        const StabStatus st = (StabStatus)r->tests[t].status.load();
        char* list = st == SS_FAIL ? failed : st == SS_ERROR ? errored : nullptr;
        if (!list) continue;
        const size_t len = strlen(list);
        snprintf(list + len, sizeof(failed) - len, "%s%s", len ? ", " : "", g_stab_tests[t].name);
    }
    const u64 errors = stab_total_errors();
    const u32 whea = r->whea_corrected + r->whea_uncorrected;
    if (errors || whea) {
        if (errors && whea)
            snprintf(why, size, "%llu errors (%s) and %u WHEA hardware errors", errors, failed, whea);
        else if (errors)
            snprintf(why, size, "%llu error%s in %s", errors, errors == 1 ? "" : "s", failed);
        else
            snprintf(why, size, "Windows logged %u WHEA hardware error%s (%u corrected, %u uncorrected)", whea,
                     whea == 1 ? "" : "s", r->whea_corrected, r->whea_uncorrected);
        return "fail";
    }
    if (r->limit_stop) {
        snprintf(why, size, "no errors, but the run was stopped early: %s", r->stop_reason);
        return "stopped";
    }
    if (errored[0]) {
        snprintf(why, size, "no errors, but %s couldn't run", errored);
        return "incomplete";
    }
    bool early = false;
    for (u32 t = 0; t < STAB_COUNT; ++t) early |= r->tests[t].status.load() == SS_STOPPED || (r->cfg.run[t] && r->tests[t].status.load() == SS_NOT_RUN);
    if (early) {
        snprintf(why, size, "no errors until it was stopped (%s)", r->stop_reason[0] ? r->stop_reason : "stopped");
        return "stopped";
    }
    snprintf(why, size, "no errors in %.0f s", stab_elapsed());
    return "pass";
}

bool stab_parse_ids(const char* list, StabConfig* cfg, char* err, u32 err_size) {
    for (u32 t = 0; t < STAB_COUNT; ++t) cfg->run[t] = false;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", list);
    char* ctx = nullptr;
    bool any = false;
    for (char* tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
        bool found = false;
        if (!strcmp(tok, "all")) {      // everything except the unchecked max-power load
            for (u32 t = 0; t < STAB_COUNT; ++t) cfg->run[t] = t != STAB_POWER;
            found = true;
        } else if (!strcmp(tok, "combined")) {
            cfg->run[STAB_CPU] = cfg->run[STAB_GPU] = true;
            cfg->together = true;
            found = true;
        }
        for (u32 t = 0; t < STAB_COUNT && !found; ++t)
            if (!strcmp(tok, g_stab_tests[t].id)) cfg->run[t] = found = true;
        if (!found) { snprintf(err, err_size, "unknown stability test '%s'", tok); return false; }
        any = true;
    }
    if (!any) snprintf(err, err_size, "no stability test given");
    return any;
}

// ---- report ---------------------------------------------------------------------------------------------
// Layout: app / format / kind: stability / timestamp, `system:` (same as benchmark results), `stability:` (run
// settings, verdict, WHEA), then tables sensors[] (avg / max), tests[], lp_errors[] (per logical processor for
// the per-LP tests), chunk_errors[] (non-zero only) and errors[] (the first STAB_MAX_LOG).

static const char* const SENSOR_NAMES[SH_COUNT] = { "cpu_clock", "cpu_load", "cpu_temp", "gpu_clock", "gpu_load",
                                                    "gpu_temp", "gpu_power", "drive_temp" };
static const char* const SENSOR_UNITS[SH_COUNT] = { "MHz", "%", "C", "MHz", "%", "C", "% of limit", "C" };

static bool per_lp_test(u32 t) { return t == STAB_CPU || t == STAB_CACHE || t == STAB_CORES || t == STAB_MEM; }

static void hex(char* buf, u32 size, u64 v) { snprintf(buf, size, "0x%016llX", v); }

bool stab_write_toon(const wchar_t* path) {
    if (stab_is_running()) return false;
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const StabRun* r = &g_stab;
    char stamp[32];
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_s(&tm_local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
    toon_str(f, 0, "app", "ez-pc-bench");
    toon_num(f, 0, "format", 1);
    toon_str(f, 0, "kind", "stability");
    toon_str(f, 0, "timestamp", stamp);
    results_write_system(f);

    char why[256];
    const char* verdict = stab_verdict(why, sizeof(why));
    toon_key(f, 0, "stability");
    toon_str(f, 1, "result", verdict);
    toon_str(f, 1, "why", why);
    toon_num(f, 1, "seconds", round(stab_elapsed() * 10.0) / 10.0);
    toon_str(f, 1, "mode", r->cfg.together ? "together" : "sequence");
    toon_num(f, 1, "duration_s", r->cfg.seconds);
    toon_str(f, 1, "stop_reason", r->stop_reason);
    toon_bool(f, 1, "self_test", r->cfg.inject);
    toon_str(f, 1, "cpu_kernel", g_cpu_kernel_ids[r->cfg.cpu_kernel]);
    toon_num(f, 1, "core_seconds", r->cfg.core_seconds);
    toon_str(f, 1, "gpu", r->cfg.gpu_adapter < g_sys.gpu_count ? g_sys.gpus[r->cfg.gpu_adapter].name : "");
    const char drive[2] = { r->cfg.disk_drive, 0 };
    toon_str(f, 1, "disk_drive", drive);
    toon_key(f, 1, "limits");
    toon_num(f, 2, "cpu_temp_c", r->cfg.max_cpu_c);
    toon_num(f, 2, "gpu_temp_c", r->cfg.max_gpu_c);
    toon_num(f, 2, "drive_temp_c", r->cfg.max_drive_c);
    toon_bool(f, 2, "stop_on_error", r->cfg.stop_on_error);
    toon_key(f, 1, "whea");
    toon_bool(f, 2, "monitored", r->whea_active);
    toon_num(f, 2, "corrected", r->whea_corrected);
    toon_num(f, 2, "uncorrected", r->whea_uncorrected);

    StabSensors* s = (StabSensors*)VirtualAlloc(nullptr, sizeof(StabSensors), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (s) {
        stab_sensors(s);
        toon_table(f, 0, "sensors", SH_COUNT, "name,unit,avg,max");
        for (u32 i = 0; i < SH_COUNT; ++i) {
            ToonRow row = toon_row(f, 1);
            toon_cell_str(&row, SENSOR_NAMES[i]);
            toon_cell_str(&row, SENSOR_UNITS[i]);
            toon_cell_num(&row, round(s->avg[i] * 10.0) / 10.0);
            toon_cell_num(&row, round(s->max[i] * 10.0) / 10.0);
            toon_row_end(&row);
        }
        VirtualFree(s, 0, MEM_RELEASE);
    }

    u32 tests = 0, lps = 0, chunks = 0;
    for (u32 t = 0; t < STAB_COUNT; ++t) {
        if (r->tests[t].status.load() == SS_NOT_RUN && !r->cfg.run[t]) continue;
        ++tests;
        if (per_lp_test(t) && r->tests[t].start_t > 0.0) lps += g_topo.lp_count < STAB_MAX_LP ? g_topo.lp_count : STAB_MAX_LP;
        for (u32 c = 0; c < STAB_MAX_CHUNKS; ++c) chunks += r->tests[t].chunk_errors[c].load() != 0;
    }
    toon_table(f, 0, "tests", tests, "id,name,status,seconds,errors,first_error_s,work,work_unit,size_bytes,note");
    for (u32 t = 0; t < STAB_COUNT; ++t) {
        const StabTestState* ts = &r->tests[t];
        if (ts->status.load() == SS_NOT_RUN && !r->cfg.run[t]) continue;
        ToonRow row = toon_row(f, 1);
        toon_cell_str(&row, g_stab_tests[t].id);
        toon_cell_str(&row, g_stab_tests[t].name);
        toon_cell_str(&row, stab_status_name((StabStatus)ts->status.load()));
        toon_cell_num(&row, ts->end_t > ts->start_t ? round((ts->end_t - ts->start_t) * 10.0) / 10.0 : 0.0);
        toon_cell_num(&row, (f64)ts->errors.load());
        const f64 fe = ts->first_error_s.load();
        toon_cell_num(&row, fe >= 0.0 ? round(fe * 100.0) / 100.0 : NAN);
        toon_cell_num(&row, round(ts->work.load() * 1000.0) / 1000.0);
        toon_cell_str(&row, g_stab_tests[t].work_unit);
        toon_cell_num(&row, (f64)ts->size_bytes);
        toon_cell_str(&row, ts->note);
        toon_row_end(&row);
    }
    if (lps) {
        toon_table(f, 0, "lp_errors", lps, "test,lp,core,errors,seconds");
        for (u32 t = 0; t < STAB_COUNT; ++t) {
            const StabTestState* ts = &r->tests[t];
            if (!per_lp_test(t) || ts->start_t <= 0.0 || (ts->status.load() == SS_NOT_RUN && !r->cfg.run[t])) continue;
            for (u32 lp = 0; lp < g_topo.lp_count && lp < STAB_MAX_LP; ++lp) {
                ToonRow row = toon_row(f, 1);
                toon_cell_str(&row, g_stab_tests[t].id);
                toon_cell_num(&row, lp);
                toon_cell_num(&row, g_topo.core[lp]);
                toon_cell_num(&row, ts->lp_errors[lp].load());
                toon_cell_num(&row, t == STAB_CORES ? round(ts->lp_seconds[lp].load() * 10.0) / 10.0 : NAN);
                toon_row_end(&row);
            }
        }
    }
    if (chunks) {
        toon_table(f, 0, "chunk_errors", chunks, "test,chunk,offset,bytes,errors");
        for (u32 t = 0; t < STAB_COUNT; ++t)
            for (u32 c = 0; c < STAB_MAX_CHUNKS; ++c) {
                const u32 n = r->tests[t].chunk_errors[c].load();
                if (!n) continue;
                ToonRow row = toon_row(f, 1);
                toon_cell_str(&row, g_stab_tests[t].id);
                toon_cell_num(&row, c);
                toon_cell_num(&row, (f64)(c * r->tests[t].chunk_bytes));
                toon_cell_num(&row, (f64)r->tests[t].chunk_bytes);
                toon_cell_num(&row, n);
                toon_row_end(&row);
            }
    }
    StabError log[STAB_MAX_LOG];
    const u32 n = stab_error_log(log, STAB_MAX_LOG);
    if (n) {
        toon_table(f, 0, "errors", n, "test,t,lp,chunk,address,what,expected,actual,bits");
        for (u32 i = 0; i < n; ++i) {
            const StabError* e = &log[i];
            char a[24], ex[24], ac[24], bits[24];
            hex(a, sizeof(a), e->address);
            hex(ex, sizeof(ex), e->expected);
            hex(ac, sizeof(ac), e->actual);
            hex(bits, sizeof(bits), e->expected ^ e->actual);
            ToonRow row = toon_row(f, 1);
            toon_cell_str(&row, g_stab_tests[e->test].id);
            toon_cell_num(&row, round(e->t * 1000.0) / 1000.0);
            toon_cell_num(&row, e->lp);
            toon_cell_num(&row, e->chunk);
            toon_cell_str(&row, a);
            toon_cell_str(&row, e->what);
            toon_cell_str(&row, ex);
            toon_cell_str(&row, ac);
            toon_cell_str(&row, bits);
            toon_row_end(&row);
        }
    }
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}
