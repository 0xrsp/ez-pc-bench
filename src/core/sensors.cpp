// Sensor sampling thread (see sensors.h). Each sample first reads every source into a Reading without any
// lock, then publishes under s_lock, which the UI only holds for a copy. Log rows are written after
// publishing, under s_log_lock. Thermal zones are read by a second thread: under full load their PDH query
// waits 5-20 s for OS worker threads (PLAN.md section 13) and the other sources must not wait with it; the
// sampler uses the latest zone values. While a log is active the sampler runs at HIGHEST priority so rows
// keep their interval during benchmarks (a sample costs a few ms).
#include "core/sensors.h"
#include "core/bench.h"
#include "core/emi.h"
#include "core/physdisk.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const char* const g_channel_group_names[CHG_COUNT] = { "CPU", "CPU per logical processor", "Graphics", "Temperatures", "Power" };

// Published state (s_lock).
static SensorReading s_sensors[SENSOR_MAX];
static u32 s_count;
static f64 s_last_sample;
static SRWLOCK s_lock = SRWLOCK_INIT;
static CpuLive s_cpu;
static GpuLive s_gpus[8];
static u32 s_gpu_count;
static PowerLive s_power;
static SensorChannel s_ch[CH_MAX];
static u32 s_ch_count, s_seq;
static f32 s_hist[CH_MAX][CH_HISTORY];      // ring, shared head: every channel gets a point each second
static u32 s_hist_head, s_hist_count;
static f64 s_hist_t;

static HANDLE s_thread, s_zone_thread, s_stop, s_wake;     // s_wake: sample now (a log started)
static u32 s_refs;                          // sensors_start / sensors_stop references (callers' threads)
static SRWLOCK s_ref_lock = SRWLOCK_INIT;
static std::atomic<u32> s_period_ms{ 1000 };

// ---- sources (owned by the sampling thread) -------------------------------------------------------------

struct DiskSource { HANDLE h; u32 sensor; u32 number; };
struct Sources {
    CpuReader cpu;
    GpuReader gpu;
    u32 gpu_sensor[8];          // temperature sensor per GPU (UINT32_MAX = none)
    DiskSource disks[PHYSDISK_MAX];
    u32 disk_count;
    BatteryPoller bat;          // own thread (see battery.h)
    BatteryInfo bat_info[BATTERY_MAX];  // latest copy, taken in read_all
    u32 bat_count;
    EmiReader emi;
};

// Thermal zones, written by the zone thread (s_zone_lock).
constexpr u32 ZONE_MAX = 16;
struct ZoneValues {
    u32 count;
    char inst[ZONE_MAX][64];
    f32 c[ZONE_MAX];
    bool ok[ZONE_MAX];
};
static ZoneValues s_zones;
static SRWLOCK s_zone_lock = SRWLOCK_INIT;
static std::atomic<bool> s_zones_read;     // first read done

// One sample, read without the lock.
struct Reading {
    ZoneValues zones;
    u32 lps;
    f32 util[HW_MAX_LPS], mhz[HW_MAX_LPS];
    GpuReading gpu[8];
    bool disks_paused;
    f32 disk_c[PHYSDISK_MAX], disk_warn[PHYSDISK_MAX];
    bool disk_ok[PHYSDISK_MAX];
    f64 link_t;                 // last PCIe link read
    f32 ms[5];                  // time spent per source: CPU, GPU, drives, battery, energy meters
};
static const char* const SOURCE_NAMES[5] = { "cpu", "gpu", "drives", "battery", "emi" };
constexpr f64 LINK_EVERY_S = 10.0;  // PCIe link via SetupAPI (PnP service): slow under load, changes rarely

// ---- temperature sensors (s_sensors) ------------------------------------------------------------------

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

struct ZoneSource {
    PDH_HQUERY query;
    PDH_HCOUNTER zones;
    bool precise;               // "High Precision Temperature" (0.1 K) vs "Temperature" (K)
};

static void open_zones(ZoneSource* src) {
    if (PdhOpenQueryW(nullptr, 0, &src->query) != ERROR_SUCCESS) { src->query = nullptr; return; }
    src->precise = PdhAddEnglishCounterW(src->query, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0,
                                         &src->zones) == ERROR_SUCCESS;
    if (!src->precise &&
        PdhAddEnglishCounterW(src->query, L"\\Thermal Zone Information(*)\\Temperature", 0, &src->zones) != ERROR_SUCCESS) {
        PdhCloseQuery(src->query);
        src->query = nullptr;
        return;
    }
    PdhCollectQueryData(src->query);
}

static void open_gpu_sensors(Sources* src) {      // s_lock held
    for (u32 i = 0; i < 8; ++i) src->gpu_sensor[i] = UINT32_MAX;
    for (u32 i = 0; i < src->gpu.count; ++i)
        if (src->gpu.handle[i]) src->gpu_sensor[i] = add_sensor(SG_GPU, g_sys.gpus[i].name, src->gpu.warn_c[i]);
}

static void open_disks(Sources* src) {          // s_lock held
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
        src->disks[src->disk_count++] = { h, s, d->number };
    }
}

// ---- reading (no lock) --------------------------------------------------------------------------------

static void read_zones(ZoneSource* src, ZoneValues* out) {
    out->count = 0;
    if (!src->query || PdhCollectQueryData(src->query) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(src->zones, PDH_FMT_DOUBLE, &size, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA || !size) return;
    alignas(8) static u8 buf[16384];
    if (size > sizeof(buf)) return;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf;
    if (PdhGetFormattedCounterArrayW(src->zones, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < count && out->count < ZONE_MAX; ++i) {
        const u32 z = out->count++;
        WideCharToMultiByte(CP_UTF8, 0, items[i].szName, -1, out->inst[z], sizeof(out->inst[z]), nullptr, nullptr);
        const f64 v = items[i].FmtValue.doubleValue;
        out->c[z] = (f32)(src->precise ? v / 10.0 - 273.15 : v - 273.15);
        out->ok[z] = items[i].FmtValue.CStatus == ERROR_SUCCESS && out->c[z] > -40.0f && out->c[z] < 150.0f;
    }
}

static DWORD WINAPI zone_thread(void*) {
    ZoneSource src = {};
    open_zones(&src);
    static ZoneValues v;
    do {
        read_zones(&src, &v);
        AcquireSRWLockExclusive(&s_zone_lock);
        s_zones = v;
        ReleaseSRWLockExclusive(&s_zone_lock);
        s_zones_read.store(true);
    } while (WaitForSingleObject(s_stop, 1000) == WAIT_TIMEOUT);
    if (src.query) PdhCloseQuery(src.query);
    return 0;
}

static bool disk_test_running() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    return runner_is_running() && cur >= 0 && g_benches[cur].suite == SUITE_DISK;
}

static void read_all(Sources* src, Reading* rd) {
    AcquireSRWLockShared(&s_zone_lock);
    rd->zones = s_zones;
    ReleaseSRWLockShared(&s_zone_lock);
    f64 t = timer_now(), t0 = t;
    auto lap = [&](u32 k) { const f64 now = timer_now(); rd->ms[k] = (f32)((now - t) * 1000.0); t = now; };
    rd->lps = cpu_reader_sample(&src->cpu, rd->util, rd->mhz);
    lap(0);
    gpu_reader_collect(&src->gpu);
    const bool link = rd->link_t == 0.0 || t0 - rd->link_t >= LINK_EVERY_S;
    if (link) rd->link_t = t0;
    for (u32 i = 0; i < src->gpu.count; ++i) {
        const GpuReading prev = rd->gpu[i];
        gpu_reader_read(&src->gpu, i, &rd->gpu[i], link);
        if (!link) {
            rd->gpu[i].link_gen = prev.link_gen;
            rd->gpu[i].link_width = prev.link_width;
            rd->gpu[i].link_gen_max = prev.link_gen_max;
            rd->gpu[i].link_width_max = prev.link_width_max;
        }
    }
    lap(1);
    rd->disks_paused = disk_test_running();     // keep the last values: don't add commands to the drive under test
    for (u32 i = 0; i < src->disk_count && !rd->disks_paused; ++i) {
        rd->disk_c[i] = rd->disk_warn[i] = 0.0f;
        rd->disk_ok[i] = src->disks[i].h != INVALID_HANDLE_VALUE &&
                         physdisk_temperature(src->disks[i].h, &rd->disk_c[i], &rd->disk_warn[i]);
    }
    lap(2);
    src->bat_count = battery_poll_get(&src->bat, src->bat_info);
    lap(3);
    emi_read(&src->emi);
    lap(4);
}

// ---- channels (s_lock held) ---------------------------------------------------------------------------

// Sets channel `id` (created on first use; channels keep their slot for the life of the thread). The cursor
// makes the common case (same order every sample) a single compare.
static u32 s_cursor;
static void ch_put(const char* id, const char* name, const char* label, const char* unit, ChannelGroup g, u8 flags,
                   u8 decimals, f32 v) {
    u32 i = s_cursor < s_ch_count && !strcmp(s_ch[s_cursor].id, id) ? s_cursor : UINT32_MAX;
    for (u32 k = 0; k < s_ch_count && i == UINT32_MAX; ++k)
        if (!strcmp(s_ch[k].id, id)) i = k;
    if (i == UINT32_MAX) {
        if (s_ch_count >= CH_MAX) return;
        i = s_ch_count++;
        SensorChannel* c = &s_ch[i];
        memset(c, 0, sizeof(*c));
        snprintf(c->id, sizeof(c->id), "%s", id);
        c->group = g;
        c->flags = flags;
        c->decimals = decimals;
        c->min = c->max = NAN;
        for (u32 k = 0; k < CH_HISTORY; ++k) s_hist[i][k] = NAN;
    }
    s_cursor = i + 1;
    SensorChannel* c = &s_ch[i];
    snprintf(c->name, sizeof(c->name), "%s", name);     // names may improve once a source reports (e.g. zones)
    snprintf(c->label, sizeof(c->label), "%s", label);
    snprintf(c->unit, sizeof(c->unit), "%s", unit);
    c->valid = !isnan(v);
    c->value = v;
    if (!c->valid) return;
    c->min = c->count && c->min <= v ? c->min : v;
    c->max = c->count && c->max >= v ? c->max : v;
    c->sum += v;
    ++c->count;
}

static const char* DEG_C = "\xC2\xB0""C";

static void publish_cpu(const Reading* rd) {
    if (!rd->lps) return;
    CpuLive* c = &s_cpu;
    c->lps = rd->lps;
    f64 sum = 0.0, wsum = 0.0, w = 0.0;
    f32 hi = NAN;
    for (u32 i = 0; i < rd->lps; ++i) {
        c->util[i] = rd->util[i];
        c->mhz[i] = rd->mhz[i];
        sum += rd->util[i];
        if (!isnan(rd->mhz[i]) && !(rd->mhz[i] <= hi)) hi = rd->mhz[i];
        if (rd->util[i] >= 50.0f && !isnan(rd->mhz[i])) {
            wsum += (f64)rd->mhz[i] * rd->util[i];
            w += rd->util[i];
        }
    }
    if (!isnan(hi) && !(hi <= c->max_mhz)) c->max_mhz = hi;
    c->avg_util = (f32)(sum / rd->lps);
    c->busy_mhz = w > 0.0 ? (f32)(wsum / w) : NAN;
    if (c->hist_count < SENSOR_HISTORY) c->util_hist[c->hist_count++] = c->avg_util;
    else {
        memmove(c->util_hist, c->util_hist + 1, (SENSOR_HISTORY - 1) * sizeof(f32));
        c->util_hist[SENSOR_HISTORY - 1] = c->avg_util;
    }
}

// EMI rails: the CPU ones (package, cores) go in the CPU group, the rest (graphics, DRAM, platform) in Power.
static void emi_channels(const EmiReader* emi, bool cpu) {
    char id[40], name[96], label[24];
    for (u32 i = 0; i < emi->channel_count; ++i) {
        const EmiChannel* e = &emi->ch[i];
        if (e->cpu != cpu) continue;
        snprintf(id, sizeof(id), "emi.%s", e->id);
        snprintf(name, sizeof(name), "%s power", e->name);
        const bool core = e->order >= 100 && e->order < 900;
        if (core) snprintf(label, sizeof(label), "Core %u", e->order - 100);
        else snprintf(label, sizeof(label), "%s", e->name);
        ch_put(id, name, label, "W", cpu ? CHG_CPU : CHG_POWER, core ? CHF_DETAIL : 0, 2, e->watts);
    }
}

static void cpu_channels(const EmiReader* emi) {
    const CpuLive* c = &s_cpu;
    const bool ok = c->lps > 0;
    ch_put("cpu.load", "CPU load", "CPU load", "%", CHG_CPU, 0, 1, ok ? c->avg_util : NAN);
    ch_put("cpu.busy_mhz", "CPU clock, busy cores", "CPU clock", "MHz", CHG_CPU, 0, 0, ok ? c->busy_mhz : NAN);
    f32 hi = NAN;
    for (u32 i = 0; i < c->lps; ++i) hi = !isnan(c->mhz[i]) && !(c->mhz[i] <= hi) ? c->mhz[i] : hi;
    ch_put("cpu.max_mhz", "CPU clock, fastest core", "CPU max clock", "MHz", CHG_CPU, 0, 0, hi);
    emi_channels(emi, true);
    char id[40], name[96];
    // Before the first full sample the LP count comes from sysinfo, so the channel order is the same every run.
    const u32 lps = ok ? c->lps : g_sys.cpu.threads < HW_MAX_LPS ? g_sys.cpu.threads : HW_MAX_LPS;
    for (u32 i = 0; i < lps; ++i) {
        snprintf(id, sizeof(id), "lp%u.mhz", i);
        snprintf(name, sizeof(name), "LP %u clock", i);
        ch_put(id, name, name, "MHz", CHG_LP, CHF_DETAIL, 0, ok ? c->mhz[i] : NAN);
        snprintf(id, sizeof(id), "lp%u.load", i);
        snprintf(name, sizeof(name), "LP %u load", i);
        ch_put(id, name, name, "%", CHG_LP, CHF_DETAIL, 1, ok ? c->util[i] : NAN);
    }
}

static void gpu_channels() {
    char id[40], name[96], label[24];
    for (u32 i = 0; i < s_gpu_count; ++i) {
        const GpuLive* g = &s_gpus[i];
        if (!g->opened) continue;
        const GpuReading* r = &g->r;
        struct { const char* key; const char* what; const char* brief; const char* unit; u8 dec; f32 v; } rows[] = {
            { "core_mhz", "core clock", "clock", "MHz", 0, r->core_mhz },
            { "mem_mhz", "memory clock", "mem clock", "MHz", 0, r->mem_mhz },
            { "load", "load", "load", "%", 1, r->util },
            { "vram_mib", "VRAM used", "VRAM", "MiB", 0, r->vram_used_mib },
            { "power_pct", "power (of limit)", "power", "%", 1, r->power_pct },
            { "fan_rpm", "fan", "fan", "RPM", 0, r->fan_rpm },
        };
        for (u32 k = 0; k < ARRAY_COUNT(rows); ++k) {
            snprintf(id, sizeof(id), "gpu%u.%s", i, rows[k].key);
            snprintf(name, sizeof(name), "%s %s", g->name, rows[k].what);
            snprintf(label, sizeof(label), "GPU%u %s", i, rows[k].brief);
            ch_put(id, name, label, rows[k].unit, CHG_GPU, 0, rows[k].dec, rows[k].v);
        }
    }
}

static void power_channels(const Sources* src) {
    char id[40], name[96], label[24];
    const u32 nb = src->bat_count;
    for (u32 b = 0; b < nb; ++b) {
        const BatteryStatus* s = &src->bat_info[b].status;
        char pre[8] = "bat", what[16] = "Battery";
        if (nb > 1) { snprintf(pre, sizeof(pre), "bat%u", b); snprintf(what, sizeof(what), "Battery %u", b); }
        struct { const char* key; const char* what; const char* brief; const char* unit; u8 dec; f32 v; } rows[] = {
            { "rate_w", "rate (+ charging, - discharging)", "", "W", 2, s->rate_w },
            { "voltage_v", "voltage", " volt", "V", 3, s->voltage_v },
            { "charge_pct", "charge level", " level", "%", 1, s->charge_pct },
            { "remaining_wh", "remaining", " left", "Wh", 2, s->capacity_wh },
        };
        for (u32 k = 0; k < ARRAY_COUNT(rows); ++k) {
            snprintf(id, sizeof(id), "%s.%s", pre, rows[k].key);
            snprintf(name, sizeof(name), "%s %s", what, rows[k].what);
            snprintf(label, sizeof(label), "%s%s", what, rows[k].brief);
            ch_put(id, name, label, rows[k].unit, CHG_POWER, 0, rows[k].dec, rows[k].v);
        }
        if (src->bat.has_temp[b]) {
            snprintf(id, sizeof(id), "%s.temp", pre);
            snprintf(name, sizeof(name), "%s temperature", what);
            snprintf(label, sizeof(label), "%s temp", what);
            ch_put(id, name, label, DEG_C, CHG_TEMP, 0, 1, s->temp_c);
        }
    }
    emi_channels(&src->emi, false);
}

// ---- publishing (s_lock held) -------------------------------------------------------------------------

static void publish_zones(const Reading* rd) {
    char id[40];
    for (u32 z = 0; z < rd->zones.count; ++z) {
        // Instance names look like "\_TZ.TZ01"; show "Thermal zone TZ01".
        const char* inst = rd->zones.inst[z];
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
        if (rd->zones.ok[z]) push(s, rd->zones.c[z]);
        else no_value(s, "No reading from this zone.");
        u32 n = snprintf(id, sizeof(id), "temp.");
        for (const char* p = shortname; *p && n + 1 < sizeof(id); ++p)
            id[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ? *p : '_';
        id[n] = 0;
        ch_put(id, name, shortname, DEG_C, CHG_TEMP, 0, 1, s < s_count && s_sensors[s].valid ? s_sensors[s].temp_c : NAN);
    }
}

static void publish_gpu_temps(const Sources* src, const Reading* rd) {
    char id[40], name[128], label[24];
    for (u32 i = 0; i < src->gpu.count; ++i) {
        const u32 s = src->gpu_sensor[i];
        if (s == UINT32_MAX) continue;
        const GpuReading* r = &rd->gpu[i];
        if (isnan(r->temp_c)) {
            no_value(s, "The driver doesn't report a temperature for this GPU (common for integrated GPUs, and for a "
                        "laptop GPU that is powered down while idle).");
        } else {
            push(s, r->temp_c);
            char extra[96] = "";
            if (!isnan(r->fan_rpm)) snprintf(extra, sizeof(extra), "fan %.0f RPM", r->fan_rpm);
            if (!isnan(r->power_pct)) {
                const size_t len = strlen(extra);
                snprintf(extra + len, sizeof(extra) - len, "%spower %.0f%%", len ? ", " : "", r->power_pct);
            }
            snprintf(s_sensors[s].detail, sizeof(s_sensors[s].detail), "%s", extra);
        }
        snprintf(id, sizeof(id), "gpu%u.temp", i);
        snprintf(name, sizeof(name), "%s temperature", g_sys.gpus[i].name);
        snprintf(label, sizeof(label), "GPU%u temp", i);
        ch_put(id, name, label, DEG_C, CHG_TEMP, 0, 1, r->temp_c);
    }
}

static void publish_disks(const Sources* src, const Reading* rd) {
    char id[40], name[128], label[24];
    for (u32 i = 0; i < src->disk_count; ++i) {
        const u32 s = src->disks[i].sensor;
        if (!rd->disks_paused) {
            if (!rd->disk_ok[i]) {
                no_value(s, "This drive doesn't report a temperature to Windows, or reading it needs administrator rights.");
            } else {
                if (rd->disk_warn[i] > 0.0f) s_sensors[s].warn_c = rd->disk_warn[i];
                push(s, rd->disk_c[i]);
            }
        }
        snprintf(id, sizeof(id), "disk%u.temp", src->disks[i].number);
        snprintf(name, sizeof(name), "%s temperature", s_sensors[s].name);
        snprintf(label, sizeof(label), "Disk %u temp", src->disks[i].number);
        ch_put(id, name, label, DEG_C, CHG_TEMP, 0, 1, s_sensors[s].valid ? s_sensors[s].temp_c : NAN);
    }
}

static void publish_power(const Sources* src) {
    PowerLive* p = &s_power;
    p->read = true;
    p->battery_count = src->bat_count;
    memcpy(p->battery, src->bat_info, sizeof(p->battery));
    p->rail_count = src->emi.channel_count < SENSOR_RAILS ? src->emi.channel_count : SENSOR_RAILS;
    for (u32 i = 0; i < p->rail_count; ++i) {
        memcpy(p->rail_name[i], src->emi.ch[i].name, sizeof(p->rail_name[i]));
        memcpy(p->rail_raw[i], src->emi.ch[i].raw, sizeof(p->rail_raw[i]));
        p->rail_cpu[i] = src->emi.ch[i].cpu;
        snprintf(p->rail_id[i], sizeof(p->rail_id[i]), "emi.%s", src->emi.ch[i].id);
        p->rail_w[i] = src->emi.ch[i].watts;
    }
    memcpy(p->emi_status, src->emi.status, sizeof(p->emi_status));
}

static void hist_push(f32* hist, u32* count, f32 v) {
    if (*count < SENSOR_HISTORY) hist[(*count)++] = v;
    else {
        memmove(hist, hist + 1, (SENSOR_HISTORY - 1) * sizeof(f32));
        hist[SENSOR_HISTORY - 1] = v;
    }
}

static void publish(const Sources* src, const Reading* rd) {
    s_cursor = 0;
    publish_cpu(rd);
    cpu_channels(&src->emi);
    for (u32 i = 0; i < s_gpu_count; ++i) {
        GpuLive* g = &s_gpus[i];
        g->r = rd->gpu[i];
        if (!isnan(g->r.util)) hist_push(g->util_hist, &g->hist_count, g->r.util);
    }
    gpu_channels();
    publish_zones(rd);
    publish_gpu_temps(src, rd);
    publish_disks(src, rd);
    publish_power(src);
    power_channels(src);

    // One history point per second, whatever the sample period.
    const f64 now = timer_now();
    if (s_hist_count == 0 || now - s_hist_t >= 0.95) {
        s_hist_t = now;
        for (u32 i = 0; i < s_ch_count; ++i) s_hist[i][s_hist_head] = s_ch[i].valid ? s_ch[i].value : NAN;
        s_hist_head = (s_hist_head + 1) % CH_HISTORY;
        if (s_hist_count < CH_HISTORY) ++s_hist_count;
    }
    s_last_sample = now;
    ++s_seq;
}

// ---- log ----------------------------------------------------------------------------------------------

struct LogState {
    bool active, header_done;
    u32 interval_ms;
    char patterns[512];
    wchar_t path[MAX_PATH];
    FILE* f;
    u32 cols[CH_MAX], ncols;
    u32 rows;
    f64 t0, next_row, last_row;
    f32 slowest_ms;             // slowest sample while logging, and where its time went
    char slowest[128];
    char error[160];
};
static LogState s_log;
static SRWLOCK s_log_lock = SRWLOCK_INIT;

static bool wild(const char* pat, const char* pe, const char* s) {     // pattern [pat, pe) against s
    if (pat == pe) return !*s;
    if (*pat == '*') return wild(pat + 1, pe, s) || (*s && wild(pat, pe, s + 1));
    return *s == *pat && wild(pat + 1, pe, s + 1);
}

bool sensors_channel_matches(const SensorChannel* c, const char* patterns) {
    if (!patterns || !patterns[0]) return !(c->flags & CHF_DETAIL);
    if (!strcmp(patterns, "all")) return true;
    for (const char* p = patterns; *p;) {
        const char* e = strchr(p, ',');
        if (!e) e = p + strlen(p);
        while (p < e && *p == ' ') ++p;
        const char* end = e;
        while (end > p && end[-1] == ' ') --end;
        if (end > p && wild(p, end, c->id)) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

static void csv_field(FILE* f, const char* s) {
    if (!strpbrk(s, ",\"\n")) { fputs(s, f); return; }
    fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

// First row: fix the columns, write the CSV header. s_log_lock held; the sampling thread owns s_ch writes.
static void log_header() {
    s_log.ncols = 0;
    for (u32 i = 0; i < s_ch_count; ++i)
        if (sensors_channel_matches(&s_ch[i], s_log.patterns)) s_log.cols[s_log.ncols++] = i;
    s_log.header_done = true;
    fputs("Time,Seconds", s_log.f);
    for (u32 k = 0; k < s_log.ncols; ++k) {
        const SensorChannel* c = &s_ch[s_log.cols[k]];
        char col[128];
        snprintf(col, sizeof(col), "%s [%s]", c->name, c->unit);
        fputc(',', s_log.f);
        csv_field(s_log.f, col);
    }
    fputc('\n', s_log.f);
}

// Sampling thread, after publishing.
static void log_tick(const Reading* rd) {
    AcquireSRWLockExclusive(&s_log_lock);
    f32 total = 0.0f;
    for (u32 k = 0; k < 5; ++k) total += rd->ms[k];
    if (s_log.active && total > s_log.slowest_ms) {
        s_log.slowest_ms = total;
        u32 n = 0;
        for (u32 k = 0; k < 5; ++k)
            n += snprintf(s_log.slowest + n, sizeof(s_log.slowest) - n, "%s%s %.0f ms", k ? ", " : "", SOURCE_NAMES[k], rd->ms[k]);
    }
    const f64 now = timer_now();
    const f64 half = s_period_ms.load(std::memory_order_relaxed) / 2000.0;
    if (s_log.active && s_log.f && (!s_log.rows || now + half >= s_log.next_row)) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        if (!s_log.rows) {
            s_log.t0 = now;
            s_log.next_row = now;
        }
        if (!s_log.header_done) log_header();
        const f64 t = now - s_log.t0;
        fprintf(s_log.f, "%04u-%02u-%02u %02u:%02u:%02u.%03u,%.3f", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, t);
        for (u32 k = 0; k < s_log.ncols; ++k) {
            const SensorChannel* c = &s_ch[s_log.cols[k]];
            if (c->valid) fprintf(s_log.f, ",%.*f", c->decimals, c->value);
            else fputc(',', s_log.f);
        }
        fputc('\n', s_log.f);
        if (fflush(s_log.f) != 0 && !s_log.error[0]) snprintf(s_log.error, sizeof(s_log.error), "Writing the log failed (disk full?).");
        ++s_log.rows;
        s_log.last_row = now;
        s_log.next_row += s_log.interval_ms / 1000.0;
        if (s_log.next_row < now) s_log.next_row = now + s_log.interval_ms / 1000.0;     // fell behind (load): don't burst
    }
    ReleaseSRWLockExclusive(&s_log_lock);
}

bool sensors_log_start(const wchar_t* path, u32 interval_ms, const char* patterns, char* err, u32 err_size) {
    sensors_log_stop();
    LogState l = {};
    l.interval_ms = interval_ms < 100 ? 100 : interval_ms;
    snprintf(l.patterns, sizeof(l.patterns), "%s", patterns ? patterns : "");
    wcsncpy_s(l.path, path, _TRUNCATE);
    l.f = _wfopen(path, L"wb");
    if (l.f) fputs("\xEF\xBB\xBF", l.f);     // UTF-8 BOM: Excel otherwise reads the file as ANSI
    if (!l.f) {
        snprintf(err, err_size, "Can't create the log file (errno %d).", errno);
        return false;
    }
    l.active = true;
    sensors_start();
    AcquireSRWLockExclusive(&s_log_lock);
    s_log = l;
    ReleaseSRWLockExclusive(&s_log_lock);
    s_period_ms.store(l.interval_ms < 1000 ? l.interval_ms : 1000, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&s_ref_lock);
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_HIGHEST);    // keep the row interval under load
    if (s_wake) SetEvent(s_wake);
    ReleaseSRWLockExclusive(&s_ref_lock);
    if (err_size) err[0] = 0;
    return true;
}

void sensors_log_stop() {
    AcquireSRWLockExclusive(&s_log_lock);
    const bool was = s_log.active;
    if (was) {
        if (s_log.f && fclose(s_log.f) != 0 && !s_log.error[0])
            snprintf(s_log.error, sizeof(s_log.error), "Writing the log failed (disk full?).");
        s_log.f = nullptr;
        s_log.active = false;
    }
    ReleaseSRWLockExclusive(&s_log_lock);
    if (!was) return;
    s_period_ms.store(1000, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&s_ref_lock);
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    ReleaseSRWLockExclusive(&s_ref_lock);
    sensors_stop();
}

void sensors_log_status(SensorLogStatus* out) {
    memset(out, 0, sizeof(*out));
    AcquireSRWLockShared(&s_log_lock);
    out->active = s_log.active;
    out->rows = s_log.rows;
    out->columns = s_log.ncols;
    out->interval_ms = s_log.interval_ms;
    out->seconds = s_log.rows ? s_log.last_row - s_log.t0 : 0.0;
    out->slowest_ms = s_log.slowest_ms;
    memcpy(out->slowest, s_log.slowest, sizeof(out->slowest));
    WideCharToMultiByte(CP_UTF8, 0, s_log.path, -1, out->path, sizeof(out->path), nullptr, nullptr);
    memcpy(out->error, s_log.error, sizeof(out->error));
    ReleaseSRWLockShared(&s_log_lock);
}

// ---- thread -------------------------------------------------------------------------------------------

static DWORD WINAPI sensor_thread(void*) {
    static Sources src;         // large (PDH scratch pointers, EMI tables): not on the stack
    static Reading rd;
    memset(&src, 0, sizeof(src));
    cpu_reader_open(&src.cpu);
    gpu_reader_open(&src.gpu);
    battery_poll_start(&src.bat, 1000);
    emi_open(&src.emi);
    AcquireSRWLockExclusive(&s_lock);
    memset(&s_cpu, 0, sizeof(s_cpu));
    s_cpu.max_mhz = NAN;
    s_gpu_count = src.gpu.count;
    for (u32 i = 0; i < s_gpu_count; ++i) {
        memset(&s_gpus[i], 0, sizeof(s_gpus[i]));
        snprintf(s_gpus[i].name, sizeof(s_gpus[i].name), "%s", g_sys.gpus[i].name);
        s_gpus[i].opened = src.gpu.handle[i] != 0;
        s_gpus[i].vram_total_mib = (f32)(g_sys.gpus[i].dedicated_vram / MiB);
    }
    memset(&s_power, 0, sizeof(s_power));
    s_count = s_ch_count = s_hist_head = s_hist_count = 0;
    open_gpu_sensors(&src);
    open_disks(&src);
    ReleaseSRWLockExclusive(&s_lock);
    AcquireSRWLockExclusive(&s_zone_lock);
    s_zones.count = 0;
    ReleaseSRWLockExclusive(&s_zone_lock);
    s_zones_read.store(false);
    s_zone_thread = CreateThread(nullptr, 0, zone_thread, nullptr, 0, nullptr);
    // Idle, the first zone read takes some tens of ms: wait for it (briefly) so the first sample has the zones
    // and the channel order is the same every time.
    for (u32 i = 0; i < 50 && s_zone_thread && !s_zones_read.load(); ++i) Sleep(10);
    const HANDLE events[2] = { s_stop, s_wake };
    for (;;) {
        const f64 t = timer_now();
        read_all(&src, &rd);
        AcquireSRWLockExclusive(&s_lock);
        publish(&src, &rd);
        ReleaseSRWLockExclusive(&s_lock);
        log_tick(&rd);
        const f64 spent_ms = (timer_now() - t) * 1000.0;
        const u32 period = s_period_ms.load(std::memory_order_relaxed);
        const DWORD wait = spent_ms >= period ? 10 : (DWORD)(period - spent_ms);
        if (WaitForMultipleObjects(2, events, FALSE, wait) == WAIT_OBJECT_0) break;
    }

    cpu_reader_close(&src.cpu);
    gpu_reader_close(&src.gpu);
    battery_poll_stop(&src.bat);
    emi_close(&src.emi);
    if (s_zone_thread) {
        WaitForSingleObject(s_zone_thread, INFINITE);
        CloseHandle(s_zone_thread);
        s_zone_thread = nullptr;
    }
    for (u32 i = 0; i < src.disk_count; ++i)
        if (src.disks[i].h != INVALID_HANDLE_VALUE) CloseHandle(src.disks[i].h);
    return 0;
}

void sensors_start() {
    AcquireSRWLockExclusive(&s_ref_lock);
    if (s_refs++ == 0) {
        s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s_thread = CreateThread(nullptr, 0, sensor_thread, nullptr, 0, nullptr);
        if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    ReleaseSRWLockExclusive(&s_ref_lock);
}

void sensors_stop() {
    AcquireSRWLockExclusive(&s_ref_lock);
    if (s_refs && --s_refs == 0 && s_thread) {
        SetEvent(s_stop);
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        CloseHandle(s_stop);
        CloseHandle(s_wake);
        s_thread = s_stop = s_wake = nullptr;
    }
    ReleaseSRWLockExclusive(&s_ref_lock);
}

// ---- readers ------------------------------------------------------------------------------------------

u32 sensors_live(CpuLive* cpu, GpuLive* gpus) {
    AcquireSRWLockShared(&s_lock);
    *cpu = s_cpu;
    const u32 n = s_gpu_count;
    memcpy(gpus, s_gpus, n * sizeof(GpuLive));
    ReleaseSRWLockShared(&s_lock);
    return n;
}

u32 sensors_snapshot(SensorReading* out, u32 max, f64* updated) {
    AcquireSRWLockShared(&s_lock);
    const u32 n = s_count < max ? s_count : max;
    memcpy(out, s_sensors, n * sizeof(SensorReading));
    if (updated) *updated = s_last_sample > 0.0 ? timer_now() - s_last_sample : -1.0;
    ReleaseSRWLockShared(&s_lock);
    return n;
}

void sensors_power(PowerLive* out) {
    AcquireSRWLockShared(&s_lock);
    *out = s_power;
    ReleaseSRWLockShared(&s_lock);
}

u32 sensors_channels(SensorChannel* out, u32 max, u32* seq) {
    AcquireSRWLockShared(&s_lock);
    const u32 n = s_ch_count < max ? s_ch_count : max;
    memcpy(out, s_ch, n * sizeof(SensorChannel));
    if (seq) *seq = s_seq;
    ReleaseSRWLockShared(&s_lock);
    return n;
}

u32 sensors_channel_history(u32 ch, f32* out, u32 max) {
    AcquireSRWLockShared(&s_lock);
    u32 n = 0;
    if (ch < s_ch_count) {
        n = s_hist_count < max ? s_hist_count : max;
        const u32 start = (s_hist_head + CH_HISTORY - n) % CH_HISTORY;     // the newest n points
        for (u32 i = 0; i < n; ++i) out[i] = s_hist[ch][(start + i) % CH_HISTORY];
    }
    ReleaseSRWLockShared(&s_lock);
    return n;
}

void sensors_reset_stats() {
    AcquireSRWLockExclusive(&s_lock);
    for (u32 i = 0; i < s_ch_count; ++i) {
        SensorChannel* c = &s_ch[i];
        c->min = c->max = c->valid ? c->value : NAN;
        c->sum = c->valid ? c->value : 0.0;
        c->count = c->valid ? 1 : 0;
    }
    for (u32 i = 0; i < s_count; ++i)
        if (s_sensors[i].valid) s_sensors[i].min_c = s_sensors[i].max_c = s_sensors[i].temp_c;
    s_cpu.max_mhz = NAN;
    ReleaseSRWLockExclusive(&s_lock);
}
