// Sensor sampling thread (see sensors.h). Readings live in s_sensors under an SRW lock; the UI copies them.
#include "core/sensors.h"
#include "core/bench.h"
#include "core/physdisk.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <string.h>

static SensorReading s_sensors[SENSOR_MAX];
static u32 s_count;
static f64 s_last_sample;
static SRWLOCK s_lock = SRWLOCK_INIT;
static HANDLE s_thread, s_stop;

// Sources, owned by the sampling thread.
struct GpuSource { D3DKMT_HANDLE adapter; u32 sensor; };
struct DiskSource { HANDLE h; u32 sensor; };
struct Sources {
    PDH_HQUERY query;
    PDH_HCOUNTER zones;
    bool zones_precise;         // "High Precision Temperature" (0.1 K) vs "Temperature" (K)
    GpuSource gpus[8];
    u32 gpu_count;
    DiskSource disks[PHYSDISK_MAX];
    u32 disk_count;
};

static u32 add_sensor(SensorGroup g, const char* name, f32 warn_c) {
    if (s_count >= SENSOR_MAX) return UINT32_MAX;
    SensorReading* s = &s_sensors[s_count];
    memset(s, 0, sizeof(*s));
    s->group = g;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->warn_c = warn_c;
    return s_count++;
}

static void push(u32 i, f32 c) {
    if (i >= s_count) return;
    SensorReading* s = &s_sensors[i];
    if (!s->valid && s->history_count == 0) s->min_c = s->max_c = c;
    s->valid = true;
    s->note[0] = 0;
    s->temp_c = c;
    s->min_c = c < s->min_c ? c : s->min_c;
    s->max_c = c > s->max_c ? c : s->max_c;
    if (s->history_count < SENSOR_HISTORY) s->history[s->history_count++] = c;
    else {
        memmove(s->history, s->history + 1, (SENSOR_HISTORY - 1) * sizeof(f32));
        s->history[SENSOR_HISTORY - 1] = c;
    }
}

static void no_value(u32 i, const char* why) {
    if (i >= s_count) return;
    s_sensors[i].valid = false;
    snprintf(s_sensors[i].note, sizeof(s_sensors[i].note), "%s", why);
}

// ---- discovery ----------------------------------------------------------------------------------------

static void open_zones(Sources* src) {
    if (PdhOpenQueryW(nullptr, 0, &src->query) != ERROR_SUCCESS) { src->query = nullptr; return; }
    src->zones_precise = PdhAddEnglishCounterW(src->query, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0,
                                               &src->zones) == ERROR_SUCCESS;
    if (!src->zones_precise &&
        PdhAddEnglishCounterW(src->query, L"\\Thermal Zone Information(*)\\Temperature", 0, &src->zones) != ERROR_SUCCESS) {
        PdhCloseQuery(src->query);
        src->query = nullptr;
        return;
    }
    PdhCollectQueryData(src->query);
}

static void open_gpus(Sources* src) {
    for (u32 i = 0; i < g_sys.gpu_count && src->gpu_count < 8; ++i) {
        const GpuInfo* g = &g_sys.gpus[i];
        if (g->software) continue;
        D3DKMT_OPENADAPTERFROMLUID oa = {};
        oa.AdapterLuid.LowPart = g->luid_low;
        oa.AdapterLuid.HighPart = g->luid_high;
        if (D3DKMTOpenAdapterFromLuid(&oa) != 0) continue;
        D3DKMT_ADAPTER_PERFDATACAPS caps = {};
        D3DKMT_QUERYADAPTERINFO qi = {};
        qi.hAdapter = oa.hAdapter;
        qi.Type = KMTQAITYPE_ADAPTERPERFDATA_CAPS;
        qi.pPrivateDriverData = &caps;
        qi.PrivateDriverDataSize = sizeof(caps);
        const bool have_caps = D3DKMTQueryAdapterInfo(&qi) == 0;
        const f32 warn = have_caps && caps.TemperatureWarning ? caps.TemperatureWarning / 10.0f : 83.0f;
        const u32 s = add_sensor(SG_GPU, g->name, warn);
        if (s == UINT32_MAX) break;
        src->gpus[src->gpu_count++] = { oa.hAdapter, s };
    }
}

static void open_disks(Sources* src) {
    PhysDisk disks[PHYSDISK_MAX];
    const u32 n = physdisk_enum(disks, PHYSDISK_MAX);
    for (u32 i = 0; i < n; ++i) {
        const PhysDisk* d = &disks[i];
        if (d->virtual_disk) continue;                      // no sensor of its own
        char name[96];
        snprintf(name, sizeof(name), "%s", d->model);
        const u32 s = add_sensor(SG_DISK, name, 70.0f);
        if (s == UINT32_MAX) break;
        snprintf(s_sensors[s].detail, sizeof(s_sensors[s].detail), "Disk %u, %s%s%s", d->number, d->bus,
                 d->letters[0] ? ", " : "", d->letters);
        HANDLE h = physdisk_open(d->number, false);
        f32 t, w;
        if (h != INVALID_HANDLE_VALUE && !physdisk_temperature(h, &t, &w)) {     // some drivers want read access
            CloseHandle(h);
            h = physdisk_open(d->number, true);
        }
        src->disks[src->disk_count++] = { h, s };
    }
}

// ---- sampling -----------------------------------------------------------------------------------------

static void sample_zones(Sources* src) {
    if (!src->query || PdhCollectQueryData(src->query) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(src->zones, PDH_FMT_DOUBLE, &size, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA || !size) return;
    alignas(8) static u8 buf[16384];
    if (size > sizeof(buf)) return;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf;
    if (PdhGetFormattedCounterArrayW(src->zones, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < count; ++i) {
        // Instance names look like "\_TZ.TZ01"; show "Thermal zone TZ01".
        char inst[64];
        WideCharToMultiByte(CP_UTF8, 0, items[i].szName, -1, inst, sizeof(inst), nullptr, nullptr);
        const char* shortname = strrchr(inst, '.') ? strrchr(inst, '.') + 1 : inst;
        char name[96];
        snprintf(name, sizeof(name), "Thermal zone %s", shortname);
        u32 s = UINT32_MAX;
        for (u32 k = 0; k < s_count && s == UINT32_MAX; ++k)
            if (s_sensors[k].group == SG_CPU && !strcmp(s_sensors[k].name, name)) s = k;
        if (s == UINT32_MAX) {
            s = add_sensor(SG_CPU, name, 90.0f);
            if (s == UINT32_MAX) continue;
            snprintf(s_sensors[s].detail, sizeof(s_sensors[s].detail), "ACPI %s", inst);
        }
        const f64 v = items[i].FmtValue.doubleValue;
        const f32 c = (f32)(src->zones_precise ? v / 10.0 - 273.15 : v - 273.15);
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS && c > -40.0f && c < 150.0f) push(s, c);
        else no_value(s, "No reading from this zone.");
    }
}

static void sample_gpus(Sources* src) {
    for (u32 i = 0; i < src->gpu_count; ++i) {
        D3DKMT_ADAPTER_PERFDATA pd = {};
        D3DKMT_QUERYADAPTERINFO qi = {};
        qi.hAdapter = src->gpus[i].adapter;
        qi.Type = KMTQAITYPE_ADAPTERPERFDATA;
        qi.pPrivateDriverData = &pd;
        qi.PrivateDriverDataSize = sizeof(pd);
        const u32 s = src->gpus[i].sensor;
        if (D3DKMTQueryAdapterInfo(&qi) != 0 || pd.Temperature == 0) {
            no_value(s, "The driver doesn't report a temperature for this GPU (common for integrated GPUs, and for a "
                        "laptop GPU that is powered down while idle).");
            continue;
        }
        push(s, pd.Temperature / 10.0f);
        char extra[96] = "";
        if (pd.FanRPM) snprintf(extra, sizeof(extra), "fan %lu RPM", pd.FanRPM);
        if (pd.Power) {
            const size_t len = strlen(extra);
            snprintf(extra + len, sizeof(extra) - len, "%spower %.0f%%", len ? ", " : "", pd.Power / 10.0);
        }
        snprintf(s_sensors[s].detail, sizeof(s_sensors[s].detail), "%s", extra);
    }
}

static bool disk_test_running() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    return runner_is_running() && cur >= 0 && g_benches[cur].suite == SUITE_DISK;
}

static void sample_disks(Sources* src) {
    if (disk_test_running()) return;    // keep the last values: don't add commands to the drive under test
    for (u32 i = 0; i < src->disk_count; ++i) {
        const u32 s = src->disks[i].sensor;
        f32 t = 0.0f, w = 0.0f;
        if (src->disks[i].h == INVALID_HANDLE_VALUE || !physdisk_temperature(src->disks[i].h, &t, &w)) {
            no_value(s, "This drive doesn't report a temperature to Windows, or reading it needs administrator rights.");
            continue;
        }
        if (w > 0.0f) s_sensors[s].warn_c = w;
        push(s, t);
    }
}

static DWORD WINAPI sensor_thread(void*) {
    Sources src = {};
    AcquireSRWLockExclusive(&s_lock);
    s_count = 0;
    open_gpus(&src);
    open_disks(&src);
    ReleaseSRWLockExclusive(&s_lock);
    open_zones(&src);
    do {
        AcquireSRWLockExclusive(&s_lock);
        sample_zones(&src);
        sample_gpus(&src);
        sample_disks(&src);
        s_last_sample = timer_now();
        ReleaseSRWLockExclusive(&s_lock);
    } while (WaitForSingleObject(s_stop, 1000) == WAIT_TIMEOUT);

    if (src.query) PdhCloseQuery(src.query);
    for (u32 i = 0; i < src.gpu_count; ++i) {
        D3DKMT_CLOSEADAPTER ca = { src.gpus[i].adapter };
        D3DKMTCloseAdapter(&ca);
    }
    for (u32 i = 0; i < src.disk_count; ++i)
        if (src.disks[i].h != INVALID_HANDLE_VALUE) CloseHandle(src.disks[i].h);
    return 0;
}

void sensors_start() {
    if (s_thread) return;
    s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_thread = CreateThread(nullptr, 0, sensor_thread, nullptr, 0, nullptr);
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void sensors_stop() {
    if (!s_thread) return;
    SetEvent(s_stop);
    WaitForSingleObject(s_thread, INFINITE);
    CloseHandle(s_thread);
    CloseHandle(s_stop);
    s_thread = s_stop = nullptr;
}

u32 sensors_snapshot(SensorReading* out, u32 max, f64* updated) {
    AcquireSRWLockShared(&s_lock);
    const u32 n = s_count < max ? s_count : max;
    memcpy(out, s_sensors, n * sizeof(SensorReading));
    if (updated) *updated = s_last_sample > 0.0 ? timer_now() - s_last_sample : -1.0;
    ReleaseSRWLockShared(&s_lock);
    return n;
}
