// Disk latency tests (FUTURE.md 2.1): QD1 4K read / write / write + flush / FUA, the latency distribution as a
// series, QD1 read latency under background writes, and wake-up latency after idle periods.
#include "disk/disk_internal.h"
#include "disk/disk_nvme.h"
#include "core/timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char* const US = "\xC2\xB5s";

// avg, p50 .. max, then IOPS: the first metric (what the GUI tree shows) is the average latency.
static void add_latency_metrics(BenchResult* out, const char* label, const char* phase, const IoResult* r, u32 dir) {
    add_hist_metrics(out, label, phase, &r->hist[dir], IOM_AVG | IOM_PCT);
    res_add(out, dname("%s %s IOPS", label, phase), "IOPS", io_iops(r));
}

struct LatPhase {
    const char* name;
    u8 read_pct;
    bool flush, fua;
};

static void run_qd1(const LatPhase* ph, u32 nph, const BenchParams* p, BenchLive* live, BenchResult* out,
                    const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static IoResult r;
    char error[96] = {};
    const f64 seconds = p->duration_ms / 1000.0;
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        for (u32 f = 0; f < nph && !cancelled(cancel); ++f) {
            live_phase(live, dname("%s %s", t->label, ph[f].name), US);
            if (f) pause_ms(250, cancel);
            const IoSpec s = { 4096, 1, 1, true, ph[f].read_pct, ph[f].flush, ph[f].fua };
            const f32 from = (f32)(i * nph + f) / (g_disk.count * nph), to = (f32)(i * nph + f + 1) / (g_disk.count * nph);
            if (!io_run1(t, &s, seconds, LIVE_LAT_US, from, to, &r, live, cancel, error, sizeof(error))) {
                res_fail(out, "%s %s", t->label, error);
                return;
            }
            add_latency_metrics(out, t->label, ph[f].name, &r, ph[f].read_pct == 100 ? 0 : 1);
        }
    }
}

void disk_lat_read(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const LatPhase ph[] = { { "4K read", 100, false, false } };
    run_qd1(ph, 1, p, live, out, cancel);
}

void disk_lat_write(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const LatPhase ph[] = { { "4K write", 0, false, false } };
    run_qd1(ph, 1, p, live, out, cancel);
}

// The cost of an fsync: each 4K write followed by FlushFileBuffers (the drive empties its volatile write
// cache), and the same write as FUA (FILE_FLAG_WRITE_THROUGH: only this write must reach stable media).
void disk_lat_flush(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static const LatPhase ph[] = { { "4K write+flush", 0, true, false }, { "4K write FUA", 0, false, true } };
    run_qd1(ph, 2, p, live, out, cancel);
}

// ---- latency distribution (series) -------------------------------------------------------------------------
// Histogram buckets merged to 4 per octave over a fixed range (the first point holds everything below 32 QPC
// ticks, the last everything above ~2^20 ticks, 100 ms at 10 MHz), so every run has the same points and repeats /
// compares line up. x = upper edge of the group in microseconds, y = % of the I/Os.

constexpr u32 HG_PER = HIST_SUB / 4;                                    // buckets per point
constexpr u32 HG_LAST = (20 - HIST_SUB_BITS - 1) * HIST_SUB / HG_PER;   // last point (open-ended above)

static f64 round_sig3(f64 v) {
    if (v <= 0.0) return v;
    const f64 scale = pow(10.0, floor(log10(v)) - 2.0);
    return floor(v / scale + 0.5) * scale;
}

static void add_hist_series(BenchResult* out, const char* name, const LatHist* h) {
    u64 counts[HG_LAST + 1] = {};
    for (u32 b = 0; b < HIST_BUCKETS; ++b) {
        const u32 g = b < 2 * HIST_SUB ? 0 : 1 + (b - 2 * HIST_SUB) / HG_PER;
        counts[g < HG_LAST ? g : HG_LAST] += h->buckets[b];
    }
    for (u32 g = 0; g <= HG_LAST; ++g) {
        const u32 last = g == 0 ? 2 * HIST_SUB - 1 : 2 * HIST_SUB + g * HG_PER - 1;
        res_add(out, name, "%", h->count ? 100.0 * counts[g] / h->count : 0.0, round_sig3(hist_bucket_upper_us(last)));
    }
}

void disk_lat_hist(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static IoResult r[DISK_MAX_TARGETS][2];
    char error[96] = {};
    const f64 seconds = p->duration_ms / 1000.0;
    const bool do_read = p->disk.mode != DISK_MODE_WRITE, do_write = p->disk.mode != DISK_MODE_READ;
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        for (u32 d = 0; d < 2 && !cancelled(cancel); ++d) {
            memset(&r[i][d], 0, sizeof(r[i][d]));
            if (d == 0 ? !do_read : !do_write) continue;
            live_phase(live, dname("%s 4K %s QD1", t->label, d ? "write" : "read"), US);
            const IoSpec s = { 4096, 1, 1, true, (u8)(d ? 0 : 100) };
            const f32 from = (f32)(i * 2 + d) / (g_disk.count * 2), to = (f32)(i * 2 + d + 1) / (g_disk.count * 2);
            if (!io_run1(t, &s, seconds, LIVE_LAT_US, from, to, &r[i][d], live, cancel, error, sizeof(error))) {
                res_fail(out, "%s %s", t->label, error);
                return;
            }
        }
    }
    if (cancelled(cancel)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Latency (upper edge)", US);
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        if (do_read) add_hist_series(out, dname("%s 4K read QD1", t->label), &r[i][0].hist[0]);
        if (do_write) add_hist_series(out, dname("%s 4K write QD1", t->label), &r[i][1].hist[1]);
    }
}

// ---- latency under load -------------------------------------------------------------------------------------
// A QD1 4K random reader in the first half of the test file, alone and then next to background writers in the
// second half: sequential 1 MiB Q8 (a big copy / download) and random 4K Q32 (an update / database).

void disk_lat_load(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    const u64 half = g_disk.file_bytes / 2 / MiB * MiB;
    const IoSpec reader = { 4096, 1, 1, true, 100, false, false, 0, half };
    const IoSpec writers[2] = { { 1024 * 1024, 8, 1, false, 0, false, false, half, half },
                                { 4096, 32, 1, true, 0, false, false, half, half } };
    static const char* const LOAD[2] = { "seq write load", "rnd write load" };
    static IoResult r[2];
    char error[96] = {};
    const f64 seconds = p->duration_ms / 1000.0;
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        const f32 base = (f32)i / g_disk.count, step = 1.0f / (g_disk.count * 3);
        live_phase(live, dname("%s 4K read QD1, idle", t->label), US);
        if (!io_run1(t, &reader, seconds, LIVE_LAT_US, base, base + step, &r[0], live, cancel, error, sizeof(error))) {
            res_fail(out, "%s %s", t->label, error);
            return;
        }
        if (cancelled(cancel)) return;
        add_hist_metrics(out, t->label, "Read idle", &r[0].hist[0], IOM_AVG | IOM_PCT);
        for (u32 k = 0; k < 2; ++k) {
            pause_ms(500, cancel);
            if (cancelled(cancel)) return;
            live_phase(live, dname("%s 4K read QD1 under %s", t->label, LOAD[k]), US);
            const IoSpec groups[2] = { reader, writers[k] };
            IoRun run = {};
            run.groups = groups;
            run.group_count = 2;
            run.seconds = seconds;
            run.live = LIVE_LAT_US;
            run.prog_from = base + step * (1 + k);
            run.prog_to = base + step * (2 + k);
            if (!io_run(t, &run, r, live, cancel, error, sizeof(error))) {
                res_fail(out, "%s %s", t->label, error);
                return;
            }
            if (cancelled(cancel)) return;
            add_hist_metrics(out, t->label, dname("Read under %s", LOAD[k]), &r[0].hist[0], IOM_AVG | IOM_PCT);
            res_add(out, dname("%s Background %s", t->label, k ? "rnd write" : "seq write"), "MB/s", io_mbps(&r[1]));
        }
    }
}

// ---- wake-up latency -----------------------------------------------------------------------------------------
// The first read after the drive sat idle for 1 / 5 / 30 s: NVMe drives drop into a non-operational power
// state (APST) after a short idle time and pay the exit latency on the next command. Each sample is one
// unbuffered 4K read at a random offset; the median and max of the samples are reported, next to the active
// QD1 read latency and the exit latencies the drive advertises in Identify Controller.

static const u32 WAKE_IDLE_S[] = { 1, 5, 30 };
static const u32 WAKE_SAMPLES[] = { 5, 3, 2 };

static bool timed_read(HANDLE h, u8* buf, u64 blocks, u64* rng, f64* us) {
    const u64 off = rng_next(rng) % blocks * 4096;
    OVERLAPPED o = {};
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    const u64 t0 = timer_ticks();
    const BOOL ok = ReadFile(h, buf, 4096, &got, &o);
    *us = ticks_us(timer_ticks() - t0);
    return ok && got == 4096;
}

void disk_lat_wake(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    u32 total_s = 0;
    for (u32 k = 0; k < ARRAY_COUNT(WAKE_IDLE_S); ++k) total_s += WAKE_IDLE_S[k] * WAKE_SAMPLES[k];
    u8* buf = (u8*)disk_alloc(4096);
    if (!buf) { res_fail(out, "out of memory"); return; }
    static IoResult active;
    char error[96] = {};
    res_begin(out, RESULT_VALUES);
    u64 rng = timer_ticks() | 1;
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        const f32 base = (f32)i / g_disk.count, span = 1.0f / g_disk.count;
        live_phase(live, dname("%s active 4K read QD1", t->label), US);
        const IoSpec s = { 4096, 1, 1, true, 100 };
        if (!io_run1(t, &s, 1.0, LIVE_LAT_US, base, base + span * 0.02f, &active, live, cancel, error, sizeof(error))) {
            res_fail(out, "%s %s", t->label, error);
            break;
        }
        res_add(out, dname("%s Active read p50", t->label), US, hist_percentile_us(&active.hist[0], 50.0));

        HANDLE h = CreateFileW(t->path, GENERIC_READ, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if (h == INVALID_HANDLE_VALUE) { res_fail(out, "%s cannot open test file (error %lu)", t->label, GetLastError()); break; }
        u32 slept_s = 0;
        for (u32 k = 0; k < ARRAY_COUNT(WAKE_IDLE_S) && !cancelled(cancel); ++k) {
            f64 v[8];
            u32 n = 0;
            live_phase(live, dname("%s first read after %u s idle", t->label, WAKE_IDLE_S[k]), US);
            for (u32 j = 0; j < WAKE_SAMPLES[k] && !cancelled(cancel); ++j) {
                for (u32 ms = 0; ms < WAKE_IDLE_S[k] * 1000 && !cancelled(cancel); ms += 100) {
                    Sleep(100);
                    live_progress(live, base + span * (0.02f + 0.98f * (slept_s * 1000 + ms) / (total_s * 1000.0f)));
                }
                slept_s += WAKE_IDLE_S[k];
                if (cancelled(cancel)) break;
                f64 us;
                if (!timed_read(h, buf, g_disk.file_bytes / 4096, &rng, &us)) {
                    res_fail(out, "%s ReadFile failed (error %lu)", t->label, GetLastError());
                    CloseHandle(h);
                    disk_free(buf);
                    return;
                }
                live_sample(live, us);
                u32 at = n++;       // insertion keeps v sorted
                for (; at > 0 && v[at - 1] > us; --at) v[at] = v[at - 1];
                v[at] = us;
            }
            if (cancelled(cancel) || !n) break;
            const f64 median = n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
            res_add(out, dname("%s After %u s idle", t->label, WAKE_IDLE_S[k]), US, median);
            res_add(out, dname("%s After %u s idle max", t->label, WAKE_IDLE_S[k]), US, v[n - 1]);
        }
        CloseHandle(h);

        NvmePowerState ps[32];
        bool apst = false;
        const u32 nps = nvme_power_states(t->phys_disk, ps, ARRAY_COUNT(ps), &apst);
        for (u32 k = 0; k < nps; ++k) {
            if (!ps[k].non_operational) continue;
            res_add(out, dname("%s PS%u exit latency (spec)", t->label, ps[k].index), US, ps[k].exit_lat_us);
            res_add(out, dname("%s PS%u entry latency (spec)", t->label, ps[k].index), US, ps[k].entry_lat_us);
        }
    }
    disk_free(buf);
}
