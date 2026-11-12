// Run monitor (see monitor.h). One sampling thread per run; per-test aggregates are published to g_env
// when the runner moves on to the next test (or the run ends).
#include "core/monitor.h"
#include "core/adv_sensors.h"
#include "core/hwread.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TestEnv g_env[MAX_BENCHES];
static std::atomic<bool> s_env_ready[MAX_BENCHES];
static HANDLE s_thread, s_stop;
static u32 s_gpu_adapter;
static bool s_run_on_battery;      // the run started on battery: the banner already says so, tests don't repeat it

constexpr u32 SAMPLE_MS = 250;
constexpr u32 SERIES_MAX = 8192;            // per test, ~34 minutes at 4 Hz; later samples only count in the averages
constexpr f32 DROP_FLAG_PCT = 10.0f;
constexpr f32 CPU_HOT_C = 95.0f;

// ---- sampling -------------------------------------------------------------------------------------------
// Readers in hwread.*. The CPU clock query stalls under all-core load (see hwread.h), so samples get further
// apart during multi-thread tests; averages are time-weighted for that reason. The ACPI thermal-zone counter
// stalls for 5-20 s under load and on many laptops doesn't follow the CPU, so CPU temperature comes only from the
// PawnIO sensors (adv_sensors.h, 1 Hz); without PawnIO cpu_temp_max stays NaN.

struct Sources {
    CpuReader cpu;
    GpuReader gpu;
    f64 last_t;                 // time of the previous sample
};

struct Sample {
    f32 cpu_mhz, cpu_mhz_max, cpu_util, cpu_temp;
    f32 gpu_mhz, gpu_mem_mhz, gpu_util, gpu_temp, gpu_power;
    bool on_ac;
    f64 dt;                 // seconds since the previous sample (weight)
};

static void open_sources(Sources* s) {
    memset(s, 0, sizeof(*s));
    cpu_reader_open(&s->cpu);
    gpu_reader_open(&s->gpu, s_gpu_adapter);
}

static void close_sources(Sources* s) {
    cpu_reader_close(&s->cpu);
    gpu_reader_close(&s->gpu);
}

static void sample_cpu(Sources* s, Sample* out) {
    out->cpu_mhz = out->cpu_mhz_max = out->cpu_util = NAN;
    out->cpu_temp = adv_cpu_temp(2.5f);     // NaN without PawnIO or when the 1 Hz sample is stale
    f32 util[HW_MAX_LPS], mhz[HW_MAX_LPS];
    const u32 n = cpu_reader_sample(&s->cpu, util, mhz);
    if (!n) return;
    // Clock of the busy LPs (>= 50 % utilization), utilization-weighted: idle LPs report parked or stale
    // clocks that say nothing about the test.
    f64 sum = 0.0, wsum = 0.0, w = 0.0;
    for (u32 i = 0; i < n; ++i) {
        sum += util[i];
        if (util[i] < 50.0f || isnan(mhz[i])) continue;
        wsum += (f64)mhz[i] * util[i];
        w += util[i];
        if (!(mhz[i] <= out->cpu_mhz_max)) out->cpu_mhz_max = mhz[i];
    }
    out->cpu_util = (f32)(sum / n);
    if (w > 0.0) out->cpu_mhz = (f32)(wsum / w);
}

static void sample_gpu(Sources* s, Sample* out) {
    GpuReading g;
    gpu_reader_read(&s->gpu, s_gpu_adapter, &g, false);
    out->gpu_mhz = g.core_mhz;
    out->gpu_mem_mhz = g.mem_mhz;
    out->gpu_util = g.util;
    out->gpu_temp = g.temp_c;
    out->gpu_power = g.power_pct;
}

static void take_sample(Sources* s, Sample* out) {
    gpu_reader_collect(&s->gpu);
    sample_cpu(s, out);
    sample_gpu(s, out);
    SYSTEM_POWER_STATUS ps;
    out->on_ac = !GetSystemPowerStatus(&ps) || ps.ACLineStatus != 0;
    const f64 now = timer_now();
    out->dt = s->last_t > 0.0 ? now - s->last_t : SAMPLE_MS / 1000.0;
    s->last_t = now;
}

// ---- per-test accumulation ----------------------------------------------------------------------------

// Averages are time-weighted: a sample covers the time since the previous one, and samples get further
// apart under all-core load (see the sources note above).
struct Stat {
    f64 sum, weight;
    u32 n;
    f32 max;
};
static void stat_add(Stat* s, f32 v, f64 w) {
    if (isnan(v)) return;
    s->sum += v * w;
    s->weight += w;
    s->max = s->n == 0 || v > s->max ? v : s->max;
    ++s->n;
}
static f32 stat_avg(const Stat* s) { return s->weight > 0.0 ? (f32)(s->sum / s->weight) : NAN; }
static f32 stat_max(const Stat* s) { return s->n ? s->max : NAN; }

// Clock trend within each phase of a test (a test's phases differ on purpose: single-thread boosts higher
// than all-core, so the drop is measured per phase and the worst phase counts).
struct Trend {
    const char* phase;
    f32 series[SERIES_MAX];
    u32 n;
    f32 worst;              // largest % drop of a finished phase, NaN if none had enough samples
};

// % drop from the average of the first third to the average of the last third (needs >= 9 samples: under
// all-core load single samples swing ~10 %).
static f32 series_drop(const f32* v, u32 n) {
    if (n < 9) return NAN;
    const u32 third = n / 3;
    f64 a = 0.0, b = 0.0;
    for (u32 i = 0; i < third; ++i) {
        a += v[i];
        b += v[n - third + i];
    }
    return a > 0.0 ? (f32)((a - b) / a * 100.0) : NAN;
}

static void trend_close(Trend* t) {
    const f32 d = series_drop(t->series, t->n);
    if (!isnan(d) && !(d <= t->worst)) t->worst = d;
    t->n = 0;
}

static void trend_add(Trend* t, const char* phase, f32 v) {
    if (phase != t->phase) {     // the first sample of a phase still covers part of the previous one: skip it
        trend_close(t);
        t->phase = phase;
        return;
    }
    if (t->n < SERIES_MAX) t->series[t->n++] = v;
}

struct Acc {
    i32 id;
    u32 samples;
    Stat cpu_mhz, cpu_mhz_max, cpu_util, cpu_temp, gpu_mhz, gpu_mem, gpu_util, gpu_temp, gpu_power;
    bool opened;            // first (partial) sample seen
    bool ac_seen, battery_seen;
    Trend cpu_trend, gpu_trend;
};
static Acc s_acc;
static SRWLOCK s_acc_lock = SRWLOCK_INIT;
static f32 s_gpu_warn_c = 87.0f;


static void acc_reset(Acc* a, i32 id) {
    memset(a, 0, sizeof(*a));
    a->id = id;
    a->cpu_trend.worst = a->gpu_trend.worst = NAN;
}

static void acc_publish(Acc* a, f32 gpu_warn_c) {
    if (a->id < 0 || a->samples == 0) return;
    TestEnv e = {};
    e.samples = a->samples;
    e.cpu_mhz = stat_avg(&a->cpu_mhz);
    e.cpu_mhz_max = stat_max(&a->cpu_mhz_max);
    e.cpu_util = stat_avg(&a->cpu_util);
    e.cpu_temp_max = stat_max(&a->cpu_temp);
    e.gpu_mhz = stat_avg(&a->gpu_mhz);
    e.gpu_mhz_max = stat_max(&a->gpu_mhz);
    e.gpu_mem_mhz = stat_avg(&a->gpu_mem);
    e.gpu_util = stat_avg(&a->gpu_util);
    e.gpu_temp_max = stat_max(&a->gpu_temp);
    e.gpu_power = stat_avg(&a->gpu_power);
    trend_close(&a->cpu_trend);
    trend_close(&a->gpu_trend);
    e.cpu_drop = a->cpu_trend.worst;
    e.gpu_drop = a->gpu_trend.worst;
    if (a->battery_seen) e.flags |= ENV_BATTERY;
    if (a->battery_seen && a->ac_seen) e.flags |= ENV_POWER_CHANGED;
    if (e.cpu_drop >= DROP_FLAG_PCT) e.flags |= ENV_CPU_DROP;
    if (e.gpu_drop >= DROP_FLAG_PCT) e.flags |= ENV_GPU_DROP;
    if (e.cpu_temp_max >= CPU_HOT_C) e.flags |= ENV_CPU_HOT;
    if (e.gpu_temp_max >= gpu_warn_c) e.flags |= ENV_GPU_HOT;
    g_env[a->id] = e;
    s_env_ready[a->id].store(true, std::memory_order_release);
}

static void acc_add(Acc* a, const Sample* s, const char* phase) {
    ++a->samples;
    stat_add(&a->cpu_util, s->cpu_util, s->dt);
    stat_add(&a->cpu_temp, s->cpu_temp, s->dt);
    stat_add(&a->gpu_mem, s->gpu_mem_mhz, s->dt);
    stat_add(&a->gpu_util, s->gpu_util, s->dt);
    stat_add(&a->gpu_temp, s->gpu_temp, s->dt);
    stat_add(&a->gpu_power, s->gpu_power, s->dt);
    // Clocks only count while that unit is busy: an idle CPU during a GPU test (or vice versa) says nothing.
    if (!isnan(s->cpu_mhz)) {
        stat_add(&a->cpu_mhz, s->cpu_mhz, s->dt);
        stat_add(&a->cpu_mhz_max, s->cpu_mhz_max, s->dt);
        trend_add(&a->cpu_trend, phase, s->cpu_mhz);
    }
    if (!isnan(s->gpu_mhz) && !(s->gpu_util < 20.0f)) {
        stat_add(&a->gpu_mhz, s->gpu_mhz, s->dt);
        trend_add(&a->gpu_trend, phase, s->gpu_mhz);
    }
    a->ac_seen |= s->on_ac;
    a->battery_seen |= !s->on_ac;
}

static DWORD WINAPI monitor_thread(void*) {
    Sources src;
    open_sources(&src);
    s_gpu_warn_c = s_gpu_adapter < src.gpu.count ? src.gpu.warn_c[s_gpu_adapter] : 87.0f;
    while (WaitForSingleObject(s_stop, SAMPLE_MS) == WAIT_TIMEOUT) {
        Sample smp;
        take_sample(&src, &smp);
        // Attribute the sample to the running test; suite setup and the gaps between tests don't count.
        const i32 cur = g_run.current.load(std::memory_order_relaxed);
        const char* phase = g_live.phase.load(std::memory_order_relaxed);
        if (cur < 0 || (BenchStatus)g_status[cur].load(std::memory_order_acquire) != ST_RUNNING ||
            (phase && !strcmp(phase, "Suite setup")))
            continue;
        AcquireSRWLockExclusive(&s_acc_lock);
        if (cur != s_acc.id && !s_env_ready[cur].load(std::memory_order_relaxed)) {
            acc_publish(&s_acc, s_gpu_warn_c);
            acc_reset(&s_acc, cur);
        }
        // The first sample of a test spans the end of the previous one (rate counters): it only opens the test.
        if (cur == s_acc.id) {
            if (s_acc.opened) acc_add(&s_acc, &smp, phase);
            s_acc.opened = true;
        }
        ReleaseSRWLockExclusive(&s_acc_lock);
    }
    monitor_test_end(-1);
    close_sources(&src);
    return 0;
}

void monitor_test_end(i32 id) {
    AcquireSRWLockExclusive(&s_acc_lock);
    if (id < 0 || s_acc.id == id) {
        acc_publish(&s_acc, s_gpu_warn_c);
        acc_reset(&s_acc, -1);
    }
    ReleaseSRWLockExclusive(&s_acc_lock);
}

void monitor_reset() {
    memset(g_env, 0, sizeof(g_env));
    for (u32 i = 0; i < MAX_BENCHES; ++i) s_env_ready[i].store(false, std::memory_order_relaxed);
}

void monitor_begin(u32 gpu_adapter) {
    if (s_thread) return;
    s_gpu_adapter = gpu_adapter;
    SYSTEM_POWER_STATUS ps;
    s_run_on_battery = GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0;
    acc_reset(&s_acc, -1);
    adv_start();            // PawnIO CPU temperature (no-op without PawnIO)
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_thread = CreateThread(nullptr, 0, monitor_thread, nullptr, 0, nullptr);
    // Short bursts (~2 ms every 250 ms) that must not starve behind HIGHEST-priority benchmark threads.
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

void monitor_end() {
    if (!s_thread) return;
    SetEvent(s_stop);
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    CloseHandle(s_stop);
    s_thread = s_stop = nullptr;
    adv_stop();
}

// ---- formatting ---------------------------------------------------------------------------------------

bool env_valid(const TestEnv* e) {
    const ptrdiff_t id = e - g_env;
    if (id >= 0 && id < (ptrdiff_t)MAX_BENCHES) return s_env_ready[id].load(std::memory_order_acquire) && e->samples > 0;
    return e->samples > 0;      // a copy (e.g. loaded from a file)
}

bool env_is_gpu_test(u32 bench_id) {
    const SuiteId s = g_benches[bench_id].suite;
    return s == SUITE_GPU_VK || s == SUITE_GPU_D3D12;
}

static void append(char* buf, u32 size, const char* fmt, ...) {
    const size_t len = strlen(buf);
    if (len + 1 >= size) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);
}

void env_describe(const TestEnv* e, bool gpu, char* buf, u32 size) {
    buf[0] = 0;
    if (!e->samples) return;
    append(buf, size, "CPU");
    if (!isnan(e->cpu_mhz)) {
        append(buf, size, " %.2f GHz", e->cpu_mhz / 1000.0f);
        if (!isnan(e->cpu_mhz_max)) append(buf, size, " (max %.2f)", e->cpu_mhz_max / 1000.0f);
        append(buf, size, ",");
    }
    if (!isnan(e->cpu_util)) append(buf, size, " %.0f%% busy", e->cpu_util);
    if (!isnan(e->cpu_temp_max)) append(buf, size, ", %.0f \xC2\xB0""C", e->cpu_temp_max);
    if (!gpu) return;
    append(buf, size, "  \xC2\xB7  GPU");
    const size_t start = strlen(buf);
    if (!isnan(e->gpu_mhz)) append(buf, size, " %.2f GHz,", e->gpu_mhz / 1000.0f);
    if (!isnan(e->gpu_mem_mhz)) append(buf, size, " mem %.2f GHz,", e->gpu_mem_mhz / 1000.0f);
    if (!isnan(e->gpu_util)) append(buf, size, " %.0f%% busy,", e->gpu_util);
    if (!isnan(e->gpu_temp_max)) append(buf, size, " %.0f \xC2\xB0""C,", e->gpu_temp_max);
    if (!isnan(e->gpu_power)) append(buf, size, " power %.0f%%,", e->gpu_power);
    const size_t len = strlen(buf);
    if (len > start && buf[len - 1] == ',') buf[len - 1] = 0;
    else if (len == start) append(buf, size, " n/a");
}

void env_brief(const TestEnv* e, bool gpu, char* buf, u32 size) {
    buf[0] = 0;
    if (!e->samples) return;
    if (gpu) {
        append(buf, size, "GPU");
        if (!isnan(e->gpu_mhz)) append(buf, size, " %.2f GHz,", e->gpu_mhz / 1000.0f);
        if (!isnan(e->gpu_temp_max)) append(buf, size, " %.0f \xC2\xB0""C,", e->gpu_temp_max);
        else if (!isnan(e->gpu_util)) append(buf, size, " %.0f%% busy,", e->gpu_util);
    } else {
        append(buf, size, "CPU");
        if (!isnan(e->cpu_mhz)) append(buf, size, " %.2f GHz,", e->cpu_mhz / 1000.0f);
        if (!isnan(e->cpu_util)) append(buf, size, " %.0f%% busy,", e->cpu_util);
    }
    const size_t len = strlen(buf);
    if (buf[len - 1] == ',') buf[len - 1] = 0;
    else buf[0] = 0;        // nothing reported
}

void env_warnings(const TestEnv* e, char* buf, u32 size) {
    buf[0] = 0;
    if (!e->samples) return;
    const char* sep = "";
    if (e->flags & ENV_POWER_CHANGED) { append(buf, size, "power source changed"); sep = ", "; }
    else if ((e->flags & ENV_BATTERY) && !s_run_on_battery) { append(buf, size, "on battery"); sep = ", "; }
    if (e->flags & ENV_CPU_DROP) { append(buf, size, "%sCPU clock -%.0f%%", sep, e->cpu_drop); sep = ", "; }
    if (e->flags & ENV_GPU_DROP) { append(buf, size, "%sGPU clock -%.0f%%", sep, e->gpu_drop); sep = ", "; }
    if (e->flags & ENV_CPU_HOT) { append(buf, size, "%sCPU %.0f \xC2\xB0""C", sep, e->cpu_temp_max); sep = ", "; }
    if (e->flags & ENV_GPU_HOT) append(buf, size, "%sGPU %.0f \xC2\xB0""C", sep, e->gpu_temp_max);
}

static const char* const s_flag_names[] = { "battery", "power-changed", "cpu-drop", "gpu-drop", "cpu-hot", "gpu-hot" };

void env_flag_names(u32 flags, char* buf, u32 size) {
    buf[0] = 0;
    for (u32 i = 0; i < ARRAY_COUNT(s_flag_names); ++i)
        if (flags & (1u << i)) append(buf, size, "%s%s", buf[0] ? "|" : "", s_flag_names[i]);
}

u32 env_parse_flags(const char* names) {
    u32 flags = 0;
    for (u32 i = 0; i < ARRAY_COUNT(s_flag_names); ++i) {
        const size_t n = strlen(s_flag_names[i]);
        for (const char* p = strstr(names, s_flag_names[i]); p; p = strstr(p + 1, s_flag_names[i]))
            if ((p == names || p[-1] == '|') && (p[n] == 0 || p[n] == '|')) flags |= 1u << i;
    }
    return flags;
}
