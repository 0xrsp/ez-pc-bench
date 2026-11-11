// TRIM / idle recovery: how sequential speed suffers after random-write torture and how much comes back after
// idle time (the drive's garbage collection) and after TRIM + a sequential rewrite.
#include "disk/disk_internal.h"
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

// ---- TRIM / idle recovery ----------------------------------------------------------------------------------------
// A defined start (TRIM the test file, rewrite it sequentially), then sequential read / write speed: fresh, right
// after a random-write torture phase, after 30 s idle (the drive's own garbage collection), and after TRIM + a
// sequential rewrite (which also leaves the file as later tests expect it).

constexpr u32 RECOVERY_IDLE_MS = 30000;

static bool trim_file(const DiskTarget* t) {
    FILE_LEVEL_TRIM trim = {};
    trim.NumRanges = 1;
    trim.Ranges[0].Offset = 0;
    trim.Ranges[0].Length = g_disk.file_bytes;
    FILE_LEVEL_TRIM_OUTPUT res = {};
    DWORD got = 0;
    return DeviceIoControl(t->hold, FSCTL_FILE_LEVEL_TRIM, &trim, sizeof(trim), &res, sizeof(res), &got, nullptr) &&
           res.NumRangesProcessed == 1;
}

static bool rewrite_file(const DiskTarget* t, BenchLive* live, const CancelFlag* cancel, char* error, u32 error_size) {
    const u32 block = 1024 * 1024;
    const IoSpec s = { block, 8, 1, false, 0, false, false, 0, 0, g_disk.file_bytes / block };
    IoRun run = {};
    run.groups = &s;
    run.group_count = 1;
    run.seconds = 3600.0;
    run.zero_pct = disk_zero_pct();
    static IoResult r;
    static const CancelFlag never(false);
    const f32 prog = live->progress.load(std::memory_order_relaxed);
    run.prog_from = run.prog_to = prog;
    (void)cancel;
    return io_run(t, &run, &r, live, &never, error, error_size);
}

void disk_recovery(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    const f64 seconds = p->duration_ms / 1000.0;
    const IoSpec seq_read = { 1024 * 1024, 8, 1, false, 100 };
    const IoSpec seq_write = { 1024 * 1024, 8, 1, false, 0 };
    const IoSpec torture = { 4096, 32, 4, true, 0 };
    static IoResult r;
    char error[96] = {};
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        const f32 base = (f32)i / g_disk.count, span = 1.0f / g_disk.count;
        const f64 total_s = seconds * 9 + RECOVERY_IDLE_MS / 1000.0;
        f64 elapsed = 0.0;
        auto measure = [&](const IoSpec* s, f64 secs, const char* phase, const char* metric, LiveKind lk) -> bool {
            live_phase(live, dname("%s %s", t->label, phase), lk == LIVE_IOPS ? "IOPS" : "MB/s");
            const f32 from = base + span * (f32)(elapsed / total_s);
            elapsed += secs;
            const f32 to = base + span * (f32)(elapsed / total_s);
            if (!io_run1(t, s, secs, lk, from, to, &r, live, cancel, error, sizeof(error))) return false;
            if (!cancelled(cancel))
                res_add(out, dname("%s %s", t->label, metric), lk == LIVE_IOPS ? "IOPS" : "MB/s",
                        lk == LIVE_IOPS ? io_iops(&r) : io_mbps(&r));
            return !cancelled(cancel);
        };
        live_phase(live, dname("%s TRIM + sequential rewrite", t->label), "MB/s");
        bool trimmed = trim_file(t);
        bool ok = rewrite_file(t, live, cancel, error, sizeof(error));
        ok = ok && measure(&seq_read, seconds, "fresh: sequential read", "Fresh seq read", LIVE_MBS);
        ok = ok && measure(&seq_write, seconds, "fresh: sequential write", "Fresh seq write", LIVE_MBS);
        ok = ok && measure(&torture, seconds * 3, "random 4K write torture (Q32T4)", "Torture rnd write", LIVE_IOPS);
        ok = ok && measure(&seq_read, seconds, "after torture: sequential read", "After torture seq read", LIVE_MBS);
        ok = ok && measure(&seq_write, seconds, "after torture: sequential write", "After torture seq write", LIVE_MBS);
        if (ok) {
            live_phase(live, dname("%s idle 30 s", t->label), "");
            for (u32 ms = 0; ms < RECOVERY_IDLE_MS && !cancelled(cancel); ms += 100) {
                Sleep(100);
                live_progress(live, base + span * (f32)((elapsed + ms / 1000.0) / total_s));
            }
            elapsed += RECOVERY_IDLE_MS / 1000.0;
        }
        ok = ok && !cancelled(cancel);
        ok = ok && measure(&seq_read, seconds, "after idle: sequential read", "After idle seq read", LIVE_MBS);
        ok = ok && measure(&seq_write, seconds, "after idle: sequential write", "After idle seq write", LIVE_MBS);
        // Always leave a sequentially written file behind, also after a cancel or error.
        live_phase(live, dname("%s TRIM + sequential rewrite", t->label), "MB/s");
        char rewrite_error[96] = {};
        trimmed = trim_file(t) && trimmed;
        const bool rewritten = rewrite_file(t, live, cancel, rewrite_error, sizeof(rewrite_error));
        if (!ok && !cancelled(cancel)) { res_fail(out, "%s %s", t->label, error); return; }
        if (!rewritten) { res_fail(out, "%s %s", t->label, rewrite_error); return; }
        if (cancelled(cancel)) return;
        const char* tag = trimmed ? "After TRIM" : "After rewrite (no TRIM)";
        ok = measure(&seq_read, seconds, "after TRIM + rewrite: sequential read", dname("%s seq read", tag), LIVE_MBS);
        if (!ok && !cancelled(cancel)) { res_fail(out, "%s %s", t->label, error); return; }
    }
}
