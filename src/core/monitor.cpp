// Run monitor (see monitor.h). One sampling thread per run; per-test aggregates are published to g_env
// when the runner moves on to the next test (or the run ends).
#include "core/monitor.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powerbase.h>
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

// ---- sources --------------------------------------------------------------------------------------------
// CPU utilization comes from SystemProcessorPerformanceInformation (per-LP idle / kernel / user times, a
// direct kernel query: < 1 ms idle). The effective clock comes from PDH "% Processor Performance" (the
// kernel's APERF/MPERF-based figure) x the nominal clock. That PDH query is served by OS worker threads that
// get little CPU while HIGHEST-priority benchmark threads fill every core: measured 0.1 ms idle, 120-260 ms
// under full load, so the sampling interval stretches to ~0.5 s during all-core tests. Timing our own add
// chain instead was tried and rejected: with the SMT sibling busy (imul) it read 1.2 GHz for a 2.45 GHz core.
// The ACPI thermal-zone counter stalls for 5-20 s under load and on many laptops doesn't follow the CPU, so
// CPU temperature waits for the PawnIO sensors (FUTURE.md section 8); cpu_temp_max stays NaN until then.

struct Sources {
    PDH_HQUERY cpu_query, gpu_query;
    PDH_HCOUNTER cpu_perf, gpu_engine;
    f32 base_mhz;               // nominal clock "% Processor Performance" is relative to
    D3DKMT_HANDLE gpu;
    u32 gpu_luid_low;
    i32 gpu_luid_high;
    f32 gpu_warn_c;
    u8* buf;                    // PDH counter array scratch
    u32 buf_size;
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION prev[64];     // processor group 0 (the query covers one group)
    bool have_prev;
    f64 last_t;                 // time of the previous sample
    f64 cpu_collect_t;          // time of the previous "% Processor Performance" collection
};

struct Sample {
    f32 cpu_mhz, cpu_mhz_max, cpu_util, cpu_temp;
    f32 gpu_mhz, gpu_mem_mhz, gpu_util, gpu_temp, gpu_power;
    bool on_ac;
    f64 dt;                 // seconds since the previous sample (weight)
};

static void open_sources(Sources* s) {
    memset(s, 0, sizeof(*s));
    s->buf_size = 512 * 1024;      // "GPU Engine" has one instance per process x engine (~400 here)
    s->buf = (u8*)VirtualAlloc(nullptr, s->buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    struct PROCESSOR_POWER_INFORMATION {        // ntpoapi.h layout, not in the user-mode SDK headers
        ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
    };
    PROCESSOR_POWER_INFORMATION ppi[64] = {};
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi, sizeof(ppi)) == 0) s->base_mhz = (f32)ppi[0].MaxMhz;

    if (PdhOpenQueryW(nullptr, 0, &s->cpu_query) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(s->cpu_query, L"\\Processor Information(*)\\% Processor Performance", 0, &s->cpu_perf) != ERROR_SUCCESS)
            s->cpu_perf = nullptr;
        PdhCollectQueryData(s->cpu_query);      // rate counters: need a first collection
    }
    if (PdhOpenQueryW(nullptr, 0, &s->gpu_query) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(s->gpu_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &s->gpu_engine) != ERROR_SUCCESS)
            s->gpu_engine = nullptr;
        PdhCollectQueryData(s->gpu_query);
    }

    if (s_gpu_adapter < g_sys.gpu_count && !g_sys.gpus[s_gpu_adapter].software) {
        const GpuInfo* g = &g_sys.gpus[s_gpu_adapter];
        D3DKMT_OPENADAPTERFROMLUID oa = {};
        oa.AdapterLuid.LowPart = g->luid_low;
        oa.AdapterLuid.HighPart = g->luid_high;
        if (D3DKMTOpenAdapterFromLuid(&oa) == 0) {
            s->gpu = oa.hAdapter;
            s->gpu_luid_low = g->luid_low;
            s->gpu_luid_high = g->luid_high;
            D3DKMT_ADAPTER_PERFDATACAPS caps = {};
            D3DKMT_QUERYADAPTERINFO qi = {};
            qi.hAdapter = s->gpu;
            qi.Type = KMTQAITYPE_ADAPTERPERFDATA_CAPS;
            qi.pPrivateDriverData = &caps;
            qi.PrivateDriverDataSize = sizeof(caps);
            s->gpu_warn_c = D3DKMTQueryAdapterInfo(&qi) == 0 && caps.TemperatureWarning ? caps.TemperatureWarning / 10.0f : 87.0f;
        }
    }
}

static void close_sources(Sources* s) {
    if (s->cpu_query) PdhCloseQuery(s->cpu_query);
    if (s->gpu_query) PdhCloseQuery(s->gpu_query);
    if (s->gpu) {
        D3DKMT_CLOSEADAPTER ca = { s->gpu };
        D3DKMTCloseAdapter(&ca);
    }
    if (s->buf) VirtualFree(s->buf, 0, MEM_RELEASE);
}

// Formatted values of a wildcard counter into s->buf; returns the item count (0 on failure).
static DWORD counter_array(Sources* s, PDH_HCOUNTER c, PDH_FMT_COUNTERVALUE_ITEM_W** items) {
    if (!c || !s->buf) return 0;
    DWORD size = s->buf_size, count = 0;
    *items = (PDH_FMT_COUNTERVALUE_ITEM_W*)s->buf;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, *items) != ERROR_SUCCESS) return 0;
    return count;
}

// "0,5" -> 5 (group 0 only, matching the utilization query); -1 for "_Total" / "0,_Total" / other groups.
static i32 lp_index(const wchar_t* name) {
    if (wcsstr(name, L"_Total")) return -1;
    const wchar_t* comma = wcschr(name, L',');
    if (!comma || _wtoi(name) != 0) return -1;
    const i32 lp = _wtoi(comma + 1);
    return lp >= 0 && lp < 64 ? lp : -1;
}

static void sample_cpu(Sources* s, Sample* out) {
    out->cpu_mhz = out->cpu_mhz_max = out->cpu_util = out->cpu_temp = NAN;
    static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION cur[64];
    ULONG len = 0;
    if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, cur, sizeof(cur), &len) != 0) return;
    const u32 n = len / sizeof(cur[0]);
    // Rate counters over < 100 ms are noise; keep accumulating until the interval is long enough.
    const f64 now = timer_now();
    const bool clock_ok = s->cpu_query && now - s->cpu_collect_t >= 0.1;
    if (clock_ok) {
        PdhCollectQueryData(s->cpu_query);
        s->cpu_collect_t = timer_now();
    }
    if (!s->have_prev || n == 0) {
        memcpy(s->prev, cur, sizeof(cur));
        s->have_prev = true;
        return;
    }
    f32 util[64];
    f64 sum = 0.0;
    for (u32 i = 0; i < n; ++i) {
        // KernelTime includes IdleTime.
        const f64 total = (f64)(cur[i].KernelTime.QuadPart - s->prev[i].KernelTime.QuadPart) +
                          (f64)(cur[i].UserTime.QuadPart - s->prev[i].UserTime.QuadPart);
        const f64 idle = (f64)(cur[i].IdleTime.QuadPart - s->prev[i].IdleTime.QuadPart);
        const f32 u = total > 0.0 ? (f32)(100.0 * (1.0 - idle / total)) : 0.0f;
        util[i] = u < 0.0f ? 0.0f : u > 100.0f ? 100.0f : u;
        sum += util[i];
    }
    memcpy(s->prev, cur, sizeof(cur));
    out->cpu_util = (f32)(sum / n);

    // Clock of the busy LPs (>= 50 % utilization), utilization-weighted: idle LPs report parked or stale
    // clocks that say nothing about the test.
    PDH_FMT_COUNTERVALUE_ITEM_W* items;
    const DWORD m = s->base_mhz > 0.0f && clock_ok ? counter_array(s, s->cpu_perf, &items) : 0;
    f64 wsum = 0.0, w = 0.0;
    for (DWORD i = 0; i < m; ++i) {
        const i32 lp = lp_index(items[i].szName);
        if (lp < 0 || (u32)lp >= n || util[lp] < 50.0f || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        const f64 pct = items[i].FmtValue.doubleValue;
        if (!(pct > 0.0 && pct < 350.0)) continue;     // rate over a near-zero interval (reads bunched up under load)
        const f32 mhz = (f32)(pct * s->base_mhz / 100.0);
        wsum += (f64)mhz * util[lp];
        w += util[lp];
        if (!(mhz <= out->cpu_mhz_max)) out->cpu_mhz_max = mhz;
    }
    if (w > 0.0) out->cpu_mhz = (f32)(wsum / w);
}

static void sample_gpu(Sources* s, Sample* out) {
    out->gpu_mhz = out->gpu_mem_mhz = out->gpu_util = out->gpu_temp = out->gpu_power = NAN;
    if (s->gpu) {
        D3DKMT_QUERYADAPTERINFO qi = {};
        qi.hAdapter = s->gpu;
        D3DKMT_NODE_PERFDATA node = {};         // node 0 = the 3D / graphics engine
        qi.Type = KMTQAITYPE_NODEPERFDATA;
        qi.pPrivateDriverData = &node;
        qi.PrivateDriverDataSize = sizeof(node);
        if (D3DKMTQueryAdapterInfo(&qi) == 0 && node.Frequency) out->gpu_mhz = (f32)(node.Frequency / 1e6);
        D3DKMT_ADAPTER_PERFDATA pd = {};
        qi.Type = KMTQAITYPE_ADAPTERPERFDATA;
        qi.pPrivateDriverData = &pd;
        qi.PrivateDriverDataSize = sizeof(pd);
        if (D3DKMTQueryAdapterInfo(&qi) == 0) {
            if (pd.MemoryFrequency) out->gpu_mem_mhz = (f32)(pd.MemoryFrequency / 1e6);
            if (pd.Temperature) out->gpu_temp = pd.Temperature / 10.0f;
            if (pd.Power) out->gpu_power = pd.Power / 10.0f;
        }
    }
    // Engine utilization: instances are "pid_<pid>_luid_0x<high>_0x<low>_phys_0_eng_<n>_engtype_<type>", one per
    // process and engine; sum per engine over processes, report the busiest engine.
    PDH_FMT_COUNTERVALUE_ITEM_W* items;
    const DWORD n = s->gpu ? counter_array(s, s->gpu_engine, &items) : 0;
    f32 engines[64] = {};
    bool any = false;
    for (DWORD i = 0; i < n; ++i) {
        const wchar_t* l = wcsstr(items[i].szName, L"luid_0x");
        const wchar_t* e = wcsstr(items[i].szName, L"_eng_");
        if (!l || !e || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        const u32 high = wcstoul(l + 7, nullptr, 16);
        const wchar_t* low_s = wcsstr(l + 7, L"_0x");
        if (!low_s || high != (u32)s->gpu_luid_high || wcstoul(low_s + 3, nullptr, 16) != s->gpu_luid_low) continue;
        const u32 eng = (u32)_wtoi(e + 5);
        if (eng < 64) engines[eng] += (f32)items[i].FmtValue.doubleValue;
        any = true;
    }
    if (any) {
        f32 hi = 0.0f;
        for (u32 i = 0; i < 64; ++i) hi = engines[i] > hi ? engines[i] : hi;
        out->gpu_util = hi > 100.0f ? 100.0f : hi;
    }
}

static void take_sample(Sources* s, Sample* out) {
    if (s->gpu_query) PdhCollectQueryData(s->gpu_query);
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
    s_gpu_warn_c = src.gpu_warn_c;
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
