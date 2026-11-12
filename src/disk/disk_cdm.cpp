// CrystalDiskMark-style rows: the built-in Default / NVMe rows, user-defined rows, the CDM profiles, and the
// small public helpers in disk.h (row text format, display units, confirmation notes).
#include "disk/disk.h"
#include "disk/disk_internal.h"
#include "core/toon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr u32 MAX_PASSES = 9;

struct Phase {
    const char* name;
    u8 read_pct;
};

static u32 row_phases(const RowSpec* row, const BenchParams* p, Phase* ph) {
    if (row->read_pct != DISK_ROW_RW) {
        const u8 r = row->read_pct;
        ph[0] = { r == 100 ? "Read" : r == 0 ? "Write" : dname("Mix %u/%u", r, 100 - r), r };
        return 1;
    }
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
static void run_row_named(const RowSpec* row, const char* prefix, const BenchParams* p, BenchLive* live,
                          BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    Phase ph[3];
    const u32 nph = row_phases(row, p, ph);
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
                live_phase(live, dname("%s %s%s pass %u/%u", t->label, prefix, ph[f].name, k + 1, passes), "MB/s");
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
            add_io_metrics(out, t->label, dname("%s%s", prefix, ph[f].name), &best[f], dir, IOM_ALL);
        }
    }
}

void disk_run_row(const RowSpec* row, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_row_named(row, "", p, live, out, cancel);
}

// ---- built-in rows (CrystalDiskMark 8 "Default" and "NVMe SSD") ---------------------------------------------

static const RowSpec R_SEQ1M_Q8T1    = { 1024 * 1024,  8, 1,  false, DISK_ROW_RW };
static const RowSpec R_SEQ1M_Q1T1    = { 1024 * 1024,  1, 1,  false, DISK_ROW_RW };
static const RowSpec R_RND4K_Q32T1   = { 4096,        32, 1,  true,  DISK_ROW_RW };
static const RowSpec R_RND4K_Q1T1    = { 4096,         1, 1,  true,  DISK_ROW_RW };
static const RowSpec R_SEQ128K_Q32T1 = { 128 * 1024,  32, 1,  false, DISK_ROW_RW };
static const RowSpec R_RND4K_Q32T16  = { 4096,        32, 16, true,  DISK_ROW_RW };

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

// ---- user-defined rows ---------------------------------------------------------------------------------------

static void size_text(u32 block, char* buf, u32 size) {
    if (block % MiB == 0) snprintf(buf, size, "%lluM", block / MiB);
    else if (block % KiB == 0) snprintf(buf, size, "%lluK", block / KiB);
    else snprintf(buf, size, "%u", block);
}

static void run_custom(u32 index, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const DiskRow* c = &p->disk.custom[index];
    if (!c->block) {
        res_skip(out, "custom row %u is empty (Settings > Disk, or --disk-custom)", index + 1);
        return;
    }
    const RowSpec row = { c->block, c->qd, c->threads, c->random, c->read_pct };
    char sz[16];
    size_text(c->block, sz, sizeof(sz));
    // The row spec goes into the metric names, so runs with different rows never compare as the same test.
    run_row_named(&row, dname("%s%s Q%uT%u ", c->random ? "RND" : "SEQ", sz, c->qd, c->threads), p, live, out, cancel);
}

void disk_custom1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) { run_custom(0, p, live, out, cancel); }
void disk_custom2(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) { run_custom(1, p, live, out, cancel); }
void disk_custom3(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) { run_custom(2, p, live, out, cancel); }
void disk_custom4(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) { run_custom(3, p, live, out, cancel); }

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == ',' || *s == '_') ++s;
    return s;
}

static bool read_uint(const char** s, u32* v) {
    char* end;
    const unsigned long x = strtoul(*s, &end, 10);
    if (end == *s || x > 0x7FFFFFFF) return false;
    *v = (u32)x;
    *s = end;
    return true;
}

bool disk_row_parse(const char* s, DiskRow* r) {
    DiskRow x = {};
    s = skip_ws(s);
    if (!*s) { *r = x; return true; }
    if (!_strnicmp(s, "SEQ", 3)) x.random = false;
    else if (!_strnicmp(s, "RND", 3)) x.random = true;
    else return false;
    s += 3;
    u32 n, qd, threads, pct = DISK_ROW_RW;
    if (!read_uint(&s, &n)) return false;
    u64 block = n;
    if (*s == 'K' || *s == 'k') { block *= KiB; ++s; }
    else if (*s == 'M' || *s == 'm') { block *= MiB; ++s; }
    if (*s == 'i') ++s;
    if (*s == 'B' || *s == 'b') ++s;
    s = skip_ws(s);
    if (*s != 'Q' && *s != 'q') return false;
    ++s;
    if (!read_uint(&s, &qd)) return false;
    s = skip_ws(s);
    if (*s != 'T' && *s != 't') return false;
    ++s;
    if (!read_uint(&s, &threads)) return false;
    s = skip_ws(s);
    if (*s == 'R' || *s == 'r') {
        ++s;
        if (!read_uint(&s, &pct) || pct > 100) return false;
        s = skip_ws(s);
    }
    if (*s) return false;
    if (block < 512 || block % 512 || block > 64 * MiB || qd < 1 || qd > IO_MAX_QD || threads < 1 || threads > 64 ||
        qd * threads > IO_MAX_WORKERS * 32)
        return false;
    x.block = (u32)block;
    x.qd = (u16)qd;
    x.threads = (u16)threads;
    x.read_pct = (u8)pct;
    *r = x;
    return true;
}

void disk_row_format(const DiskRow* r, char* buf, u32 size) {
    if (!r->block) { buf[0] = 0; return; }
    char sz[16];
    size_text(r->block, sz, sizeof(sz));
    if (r->read_pct == DISK_ROW_RW) snprintf(buf, size, "%s%s Q%uT%u", r->random ? "RND" : "SEQ", sz, r->qd, r->threads);
    else snprintf(buf, size, "%s%s Q%uT%u R%u", r->random ? "RND" : "SEQ", sz, r->qd, r->threads, r->read_pct);
}

static const char* const MODE_NAMES[] = { "rw", "read", "write", "mix" };

const char* disk_mode_name(u8 mode) { return mode < ARRAY_COUNT(MODE_NAMES) ? MODE_NAMES[mode] : MODE_NAMES[0]; }

bool disk_mode_parse(const char* s, u8* mode) {
    for (u32 i = 0; i < ARRAY_COUNT(MODE_NAMES); ++i)
        if (!_stricmp(s, MODE_NAMES[i])) { *mode = (u8)i; return true; }
    return false;
}

// ---- profiles ---------------------------------------------------------------------------------------------------

const DiskProfile g_disk_profiles[] = {
    { "default", "Default", "CrystalDiskMark Default: SEQ1M Q8T1, SEQ1M Q1T1, RND4K Q32T1, RND4K Q1T1.",
      "disk.seq1m_q8t1,disk.seq1m_q1t1,disk.rnd4k_q32t1,disk.rnd4k_q1t1" },
    { "nvme", "NVMe SSD", "CrystalDiskMark NVMe SSD: SEQ1M Q8T1, SEQ128K Q32T1, RND4K Q32T16, RND4K Q1T1.",
      "disk.seq1m_q8t1,disk.seq128k_q32t1,disk.rnd4k_q32t16,disk.rnd4k_q1t1" },
    { "peak", "Peak", "Peak performance: SEQ1M Q8T1 and RND4K Q32T16, the most the drive can do with deep queues.",
      "disk.seq1m_q8t1,disk.rnd4k_q32t16" },
    { "realworld", "Real World", "Real-world performance: SEQ1M Q1T1 and RND4K Q1T1. Desktop apps rarely queue more "
                                 "than one or two requests, so these track how fast the PC feels.",
      "disk.seq1m_q1t1,disk.rnd4k_q1t1" },
    { "demo", "Demo", "One test, SEQ1M Q8T1: the headline sequential speed.", "disk.seq1m_q8t1" },
};
const u32 g_disk_profile_count = ARRAY_COUNT(g_disk_profiles);

static bool in_list(const char* list, const char* id) {
    const size_t n = strlen(id);
    for (const char* p = strstr(list, id); p; p = strstr(p + 1, id))
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

const DiskProfile* disk_profile_find(const char* id) {
    for (u32 i = 0; i < g_disk_profile_count; ++i)
        if (!_stricmp(g_disk_profiles[i].id, id)) return &g_disk_profiles[i];
    return nullptr;
}

void disk_profile_apply(const DiskProfile* pr, bool* selected) {
    for (u32 i = 0; i < g_bench_count; ++i)
        if (g_benches[i].suite == SUITE_DISK) selected[i] = in_list(pr->ids, g_benches[i].cli_id);
}

const DiskProfile* disk_profile_match(const bool* selected) {
    for (u32 k = 0; k < g_disk_profile_count; ++k) {
        bool same = true;
        for (u32 i = 0; i < g_bench_count && same; ++i)
            if (g_benches[i].suite == SUITE_DISK) same = selected[i] == in_list(g_disk_profiles[k].ids, g_benches[i].cli_id);
        if (same) return &g_disk_profiles[k];
    }
    return nullptr;
}

// ---- display units, confirmation notes -------------------------------------------------------------------------

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
    toon_str(f, 1, "disk_data", o->data == DISK_DATA_ZERO ? "zero" : "random");
    toon_bool(f, 1, "disk_buffered", o->buffered);
    toon_num(f, 1, "disk_interval_ms", o->interval_ms);
    toon_num(f, 1, "disk_sustained_gib", o->sustained_gib);
    if (o->folder[0]) {
        char folder[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, o->folder, -1, folder, sizeof(folder), nullptr, nullptr);
        toon_str(f, 1, "disk_folder", folder);
    }
    u32 rows = 0;
    for (u32 i = 0; i < DISK_CUSTOM_ROWS; ++i) rows += o->custom[i].block != 0;
    if (!rows) return;
    fprintf(f, "  disk_custom[%u]: ", rows);
    ToonRow r = { f, 0 };
    for (u32 i = 0; i < DISK_CUSTOM_ROWS; ++i) {
        if (!o->custom[i].block) continue;
        char row[48];
        disk_row_format(&o->custom[i], row, sizeof(row));
        toon_cell_str(&r, row);
    }
    toon_row_end(&r);
}
