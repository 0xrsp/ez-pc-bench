// CrystalDiskMark-style rows (the built-in Default / NVMe rows) and the small public helpers in disk.h (mode
// names, display units, confirmation notes).
#include "disk/disk.h"
#include "disk/disk_internal.h"
#include "core/toon.h"
#include <stdio.h>
#include <string.h>

constexpr u32 MAX_PASSES = 9;

struct Phase {
    const char* name;
    u8 read_pct;
};

static u32 row_phases(const BenchParams* p, Phase* ph) {
    u32 n = 0;
    if (p->disk.mode != DISK_MODE_WRITE) ph[n++] = { "Read", 100 };
    if (p->disk.mode != DISK_MODE_READ) ph[n++] = { "Write", 0 };
    if (p->disk.mode == DISK_MODE_RW_MIX) {
        const u8 r = p->disk.mix_read_pct > 100 ? 70 : p->disk.mix_read_pct;
        ph[n++] = { dname("Mix %u/%u", r, 100 - r), r };
    }
    return n;
}

// Best pass (by MB/s) per phase, measured and reported one target at a time ("C: Read", "C: Read IOPS", ...).
void disk_run_row(const RowSpec* row, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    Phase ph[3];
    const u32 nph = row_phases(p, ph);
    const u32 passes = p->disk_passes < 1 ? 1 : p->disk_passes > MAX_PASSES ? MAX_PASSES : p->disk_passes;
    const f64 seconds = p->duration_ms / 1000.0;
    const u32 interval_ms = p->duration_ms / 4 < 1000 ? p->duration_ms / 4 : 1000;
    static IoResult best[3], r;
    res_begin(out, RESULT_VALUES);
    char error[96] = {};
    for (u32 i = 0; i < g_disk.count; ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        const f32 span = 1.0f / (g_disk.count * nph * passes);
        for (u32 f = 0; f < nph; ++f) {
            memset(&best[f], 0, sizeof(best[f]));
            for (u32 k = 0; k < passes; ++k) {
                if (cancelled(cancel)) return;
                live_phase(live, dname("%s %s pass %u/%u", t->label, ph[f].name, k + 1, passes), "MB/s");
                if (f || k) pause_ms(interval_ms, cancel);
                const IoSpec s = { row->block, row->qd, row->threads, row->random, ph[f].read_pct };
                const f32 from = ((i * nph + f) * passes + k) * span;
                if (!io_run1(t, &s, seconds, LIVE_MBS, from, from + span, &r, live, cancel, error, sizeof(error))) {
                    res_fail(out, "%s %s", t->label, error);
                    return;
                }
                if (io_mbps(&r) > io_mbps(&best[f])) best[f] = r;
            }
        }
        if (cancelled(cancel)) return;
        for (u32 f = 0; f < nph; ++f) {
            const u32 dir = ph[f].read_pct == 100 ? 0 : ph[f].read_pct == 0 ? 1 : 2;
            add_io_metrics(out, t->label, ph[f].name, &best[f], dir, IOM_ALL);
        }
    }
}

// ---- built-in rows (CrystalDiskMark 8 "Default" and "NVMe SSD") ---------------------------------------------

static const RowSpec R_SEQ1M_Q8T1    = { 1024 * 1024,  8, 1,  false };
static const RowSpec R_SEQ1M_Q1T1    = { 1024 * 1024,  1, 1,  false };
static const RowSpec R_RND4K_Q32T1   = { 4096,        32, 1,  true  };
static const RowSpec R_RND4K_Q1T1    = { 4096,         1, 1,  true  };
static const RowSpec R_SEQ128K_Q32T1 = { 128 * 1024,  32, 1,  false };
static const RowSpec R_RND4K_Q32T16  = { 4096,        32, 16, true  };

void disk_seq1m_q8t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    disk_run_row(&R_SEQ1M_Q8T1, p, live, out, cancel);
}
void disk_seq1m_q1t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    disk_run_row(&R_SEQ1M_Q1T1, p, live, out, cancel);
}
void disk_rnd4k_q32t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    disk_run_row(&R_RND4K_Q32T1, p, live, out, cancel);
}
void disk_rnd4k_q1t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    disk_run_row(&R_RND4K_Q1T1, p, live, out, cancel);
}
void disk_seq128k_q32t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    disk_run_row(&R_SEQ128K_Q32T1, p, live, out, cancel);
}
void disk_rnd4k_q32t16(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    disk_run_row(&R_RND4K_Q32T16, p, live, out, cancel);
}

// ---- mode names, display units, confirmation notes -------------------------------------------------------------

static const char* const MODE_NAMES[] = { "rw", "read", "write", "mix" };

const char* disk_mode_name(u8 mode) { return mode < ARRAY_COUNT(MODE_NAMES) ? MODE_NAMES[mode] : MODE_NAMES[0]; }

bool disk_mode_parse(const char* s, u8* mode) {
    for (u32 i = 0; i < ARRAY_COUNT(MODE_NAMES); ++i)
        if (!_stricmp(s, MODE_NAMES[i])) { *mode = (u8)i; return true; }
    return false;
}

u32 disk_unit_pick(u8 unit, const BenchResult* r) {
    const char* want = unit == DISK_UNIT_IOPS ? "IOPS" : unit == DISK_UNIT_US ? "\xC2\xB5s" : "MB/s";
    const Metric* m = res_metrics(r);
    for (u32 k = 0; k < r->count; ++k)
        if (m[k].unit && !strcmp(m[k].unit, want)) return k;
    return 0;
}

f64 disk_unit_value(u8 unit, const char* metric_unit, f64 v, const char** shown_unit) {
    *shown_unit = metric_unit;
    if (unit == DISK_UNIT_GBS && metric_unit && !strcmp(metric_unit, "MB/s")) {
        *shown_unit = "GB/s";
        return v / 1000.0;
    }
    return v;
}

void disk_confirm_note(const BenchParams* p, u32 bench, char* buf, u32 size) {
    const char* id = g_benches[bench].cli_id;
    if (!strcmp(id, "disk.sustained.write"))
        snprintf(buf, size, "Writes up to %u GiB (at most 80 %% of the free space) to each tested drive in one go. "
                            "That uses up SSD write endurance like any large copy would.", p->disk.sustained_gib);
    else if (!strcmp(id, "disk.raw.scan"))
        snprintf(buf, size, "Reads every sector of each tested drive's physical disk (read-only). About 5-10 minutes "
                            "per TB on an NVMe SSD, hours on a large hard disk. Needs administrator rights.");
    else
        snprintf(buf, size, "Runs for a long time or writes a lot of data.");
}

void disk_write_toon_params(FILE* f, const BenchParams* p) {
    const DiskOptions* o = &p->disk;
    toon_str(f, 1, "disk_mode", disk_mode_name(o->mode));
    toon_num(f, 1, "disk_mix_read_pct", o->mix_read_pct);
    toon_bool(f, 1, "disk_buffered", o->buffered);
    toon_num(f, 1, "disk_interval_ms", o->interval_ms);
    toon_num(f, 1, "disk_sustained_gib", o->sustained_gib);
    if (o->folder[0]) {
        char folder[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, o->folder, -1, folder, sizeof(folder), nullptr, nullptr);
        toon_str(f, 1, "disk_folder", folder);
    }
}
