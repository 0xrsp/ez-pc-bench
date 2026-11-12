// Sweeps (series results): queue depth (4K random IOPS and latency vs QD 1..256), block size (ATTO-style
// sequential QD4, 512 B..64 MiB) and compressibility (AS SSD-style sequential write / read vs share of zeros).
#include "disk/disk_internal.h"
#include <stdio.h>
#include <string.h>

static const char* const US = "\xC2\xB5s";

static f64 point_seconds(const BenchParams* p, f64 div, f64 min_s) {
    const f64 s = p->duration_ms / 1000.0 / div;
    return s < min_s ? min_s : s;
}

// ---- queue depth ------------------------------------------------------------------------------------------------

static const u32 SWEEP_QD[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };
constexpr u32 NQD = ARRAY_COUNT(SWEEP_QD);

void disk_sweep_qd(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static f64 iops[DISK_MAX_TARGETS][2][NQD], avg[DISK_MAX_TARGETS][2][NQD], p99[DISK_MAX_TARGETS][2][NQD];
    static IoResult r;
    const bool dir_on[2] = { p->disk.mode != DISK_MODE_WRITE, p->disk.mode != DISK_MODE_READ };
    const f64 seconds = point_seconds(p, 3.0, 0.5);
    char error[96] = {};
    const u32 steps = g_disk.count * 2 * NQD;
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        for (u32 d = 0; d < 2; ++d) {
            for (u32 q = 0; q < NQD; ++q) {
                iops[i][d][q] = avg[i][d][q] = p99[i][d][q] = 0.0;
                if (!dir_on[d] || cancelled(cancel)) continue;
                // One thread up to QD32 (as CrystalDiskMark's Q32T1), then 32 per thread: a single submitting thread
                // tops out well below what an NVMe drive can do at QD64+.
                const u32 threads = SWEEP_QD[q] <= 32 ? 1 : SWEEP_QD[q] / 32;
                const IoSpec s = { 4096, SWEEP_QD[q] / threads, threads, true, (u8)(d ? 0 : 100) };
                live_phase(live, dname("%s 4K random %s QD%u", t->label, d ? "write" : "read", SWEEP_QD[q]), "IOPS");
                const u32 step = (i * 2 + d) * NQD + q;
                if (!io_run1(t, &s, seconds, LIVE_IOPS, (f32)step / steps, (f32)(step + 1) / steps, &r, live, cancel,
                             error, sizeof(error))) {
                    res_fail(out, "%s %s", t->label, error);
                    return;
                }
                iops[i][d][q] = io_iops(&r);
                avg[i][d][q] = hist_avg_us(&r.hist[d]);
                p99[i][d][q] = hist_percentile_us(&r.hist[d], 99.0);
            }
        }
    }
    if (cancelled(cancel)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Queue depth", "QD");
    for (u32 i = 0; i < g_disk.count; ++i) {
        const char* label = g_disk.targets[i].label;
        for (u32 d = 0; d < 2; ++d) {
            if (!dir_on[d]) continue;
            const char* dn = d ? "Write" : "Read";
            for (u32 q = 0; q < NQD; ++q) res_add(out, dname("%s %s IOPS", label, dn), "IOPS", iops[i][d][q], SWEEP_QD[q]);
            for (u32 q = 0; q < NQD; ++q) res_add(out, dname("%s %s latency", label, dn), US, avg[i][d][q], SWEEP_QD[q]);
            for (u32 q = 0; q < NQD; ++q) res_add(out, dname("%s %s p99", label, dn), US, p99[i][d][q], SWEEP_QD[q]);
        }
    }
}

// ---- block size -------------------------------------------------------------------------------------------------

constexpr u32 NBS = 18;     // 512 B << 0..17 = 512 B .. 64 MiB
constexpr u32 BS_QD = 4;

void disk_sweep_block(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static f64 mbs[DISK_MAX_TARGETS][2][NBS];
    static IoResult r;
    const bool dir_on[2] = { p->disk.mode != DISK_MODE_WRITE, p->disk.mode != DISK_MODE_READ };
    const f64 seconds = point_seconds(p, 4.0, 0.4);
    char error[96] = {};
    const u32 steps = g_disk.count * 2 * NBS;
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        for (u32 d = 0; d < 2; ++d) {
            for (u32 b = 0; b < NBS; ++b) {
                mbs[i][d][b] = -1.0;    // not measured: left out of the series
                const u32 block = 512u << b;
                if (!dir_on[d] || cancelled(cancel) || block % t->logical_sector || (u64)block * BS_QD > g_disk.file_bytes)
                    continue;
                const IoSpec s = { block, BS_QD, 1, false, (u8)(d ? 0 : 100) };
                live_phase(live, dname("%s sequential %s, %u KiB blocks", t->label, d ? "write" : "read", block / 1024), "MB/s");
                const u32 step = (i * 2 + d) * NBS + b;
                if (!io_run1(t, &s, seconds, LIVE_MBS, (f32)step / steps, (f32)(step + 1) / steps, &r, live, cancel, error,
                             sizeof(error))) {
                    res_fail(out, "%s %s", t->label, error);
                    return;
                }
                mbs[i][d][b] = io_mbps(&r);
            }
        }
    }
    if (cancelled(cancel)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Block size", "bytes");
    for (u32 i = 0; i < g_disk.count; ++i)
        for (u32 d = 0; d < 2; ++d)
            for (u32 b = 0; b < NBS; ++b)
                if (mbs[i][d][b] >= 0.0)
                    res_add(out, dname("%s %s", g_disk.targets[i].label, d ? "Write" : "Read"), "MB/s", mbs[i][d][b], 512u << b);
}

// ---- compressibility ----------------------------------------------------------------------------------------------
// Writes the whole test file once (SEQ1M Q8T1) with data that is x % zeros, then reads it back once. Controllers
// that compress (older SandForce, some enterprise drives) speed up with compressible data; most current SSDs don't.
// The file gets the user's test data back at the end so later tests read what they expect.

static const u32 COMPRESS_PCT[] = { 0, 20, 40, 60, 80, 100 };
constexpr u32 NCP = ARRAY_COUNT(COMPRESS_PCT);

static bool whole_file_pass(const DiskTarget* t, bool write, u32 zero_pct, f32 from, f32 to, IoResult* r, BenchLive* live,
                            const CancelFlag* cancel, char* error, u32 error_size) {
    const u32 block = 1024 * 1024;
    const IoSpec s = { block, 8, 1, false, (u8)(write ? 0 : 100), false, false, 0, 0, g_disk.file_bytes / block };
    IoRun run = {};
    run.groups = &s;
    run.group_count = 1;
    run.seconds = 3600.0;
    run.live = LIVE_MBS;
    run.zero_pct = zero_pct;
    run.prog_from = from;
    run.prog_to = to;
    return io_run(t, &run, r, live, cancel, error, error_size);
}

void disk_sweep_compress(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static f64 mbs[DISK_MAX_TARGETS][2][NCP];
    static IoResult r;
    char error[96] = {};
    const u32 steps = g_disk.count * (2 * NCP + 1);
    bool ok = true;
    for (u32 i = 0; i < g_disk.count && ok; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        for (u32 c = 0; c < NCP && ok && !cancelled(cancel); ++c) {
            for (u32 d = 0; d < 2 && ok; ++d) {
                const bool write = d == 0;
                live_phase(live, dname("%s %s, %u %% zeros", t->label, write ? "write" : "read", COMPRESS_PCT[c]), "MB/s");
                const u32 step = i * (2 * NCP + 1) + c * 2 + d;
                ok = whole_file_pass(t, write, COMPRESS_PCT[c], (f32)step / steps, (f32)(step + 1) / steps, &r, live,
                                     cancel, error, sizeof(error));
                mbs[i][d][c] = io_mbps(&r);
            }
        }
        // Restore the test data even after a cancel: the next tests (or the next run's repeat) read this file.
        live_phase(live, dname("%s restoring the test data", t->label), "MB/s");
        const u32 step = i * (2 * NCP + 1) + 2 * NCP;
        static const CancelFlag never(false);
        char restore_error[96] = {};
        if (!whole_file_pass(t, true, disk_zero_pct(), (f32)step / steps, (f32)(step + 1) / steps, &r, live, &never,
                             restore_error, sizeof(restore_error)) && ok) {
            ok = false;
            memcpy(error, restore_error, sizeof(error));
        }
        if (!ok) res_fail(out, "%s %s", t->label, error);
    }
    if (!ok || cancelled(cancel)) return;
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Compressible (zeros)", "%");
    for (u32 i = 0; i < g_disk.count; ++i)
        for (u32 d = 0; d < 2; ++d)
            for (u32 c = 0; c < NCP; ++c)
                res_add(out, dname("%s %s", g_disk.targets[i].label, d ? "Read" : "Write"), "MB/s", mbs[i][d][c], COMPRESS_PCT[c]);
}
