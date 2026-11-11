// Comparison set: loads result files (and the current run) into one aligned table, see compare.h.
#include "core/compare.h"
#include "core/baseline.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/toon.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

CompareSet g_cmp;

const char* const g_cmp_info_names[CI_COUNT] = { "Date", "CPU", "RAM", "GPU", "Disk", "OS", "Power", "Administrator",
                                                 "Phase duration" };

static void copy_str(char* dst, u32 size, const char* src) {
    snprintf(dst, size, "%s", src ? src : "");
}

void compare_clear() {
    memset(&g_cmp, 0, sizeof(g_cmp));
}

// ---- table building ------------------------------------------------------------------------------

static u32 find_test(const char* id, const char* suite, const char* title, const char* x_unit) {
    for (u32 i = 0; i < g_cmp.test_count; ++i)
        if (!strcmp(g_cmp.tests[i].id, id)) return i;
    if (g_cmp.test_count >= CMP_MAX_TESTS) return UINT32_MAX;
    CmpTest* t = &g_cmp.tests[g_cmp.test_count];
    copy_str(t->id, sizeof(t->id), id);
    copy_str(t->suite, sizeof(t->suite), suite);
    copy_str(t->title, sizeof(t->title), title);
    copy_str(t->x_unit, sizeof(t->x_unit), x_unit);
    return g_cmp.test_count++;
}

// Row of (test, stat, name, x); a new row goes after the test's last row so tests stay grouped.
static u32 find_row(u32 test, CmpStat stat, const char* name, const char* unit, f64 x) {
    u32 after = UINT32_MAX;
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        const CmpRow* r = &g_cmp.rows[i];
        if (r->test != test) continue;
        if (r->stat == stat && r->x == x && !strcmp(r->name, name)) return i;
        after = i;
    }
    if (g_cmp.row_count >= CMP_MAX_ROWS) return UINT32_MAX;
    const u32 at = after == UINT32_MAX ? g_cmp.row_count : after + 1;
    const u32 tail = g_cmp.row_count - at;
    memmove(&g_cmp.rows[at + 1], &g_cmp.rows[at], tail * sizeof(CmpRow));
    memmove(&g_cmp.values[at + 1], &g_cmp.values[at], tail * sizeof(g_cmp.values[0]));
    CmpRow* r = &g_cmp.rows[at];
    r->test = (u16)test;
    r->stat = stat;
    copy_str(r->name, sizeof(r->name), name);
    copy_str(r->unit, sizeof(r->unit), unit);
    r->x = x;
    for (u32 c = 0; c < CMP_MAX_RUNS; ++c) g_cmp.values[at][c] = NAN;
    g_cmp.row_count++;
    return at;
}

static void add_value(u32 run, u32 test, CmpStat stat, const char* name, const char* unit, f64 x, f64 value) {
    if (test == UINT32_MAX || !isfinite(value)) return;
    const u32 row = find_row(test, stat, name, unit, x);
    if (row == UINT32_MAX) return;
    if (isnan(g_cmp.values[row][run])) g_cmp.runs[run].metrics++;
    g_cmp.values[row][run] = value;
}

static void add_matrix(u32 run, u32 test, const char* name, const char* unit, f64 lo, f64 avg, f64 hi) {
    add_value(run, test, CS_MIN, name, unit, 0.0, lo);
    add_value(run, test, CS_AVG, name, unit, 0.0, avg);
    add_value(run, test, CS_MAX, name, unit, 0.0, hi);
}

// Drops rows no run has a value for, then tests without rows.
static void compact() {
    u32 n = 0;
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        bool any = false;
        for (u32 c = 0; c < g_cmp.run_count && !any; ++c) any = !isnan(g_cmp.values[i][c]);
        if (!any) continue;
        if (n != i) {
            g_cmp.rows[n] = g_cmp.rows[i];
            memcpy(g_cmp.values[n], g_cmp.values[i], sizeof(g_cmp.values[0]));
        }
        ++n;
    }
    g_cmp.row_count = n;

    u16 remap[CMP_MAX_TESTS];
    bool used[CMP_MAX_TESTS] = {};
    for (u32 i = 0; i < g_cmp.row_count; ++i) used[g_cmp.rows[i].test] = true;
    u32 t = 0;
    for (u32 i = 0; i < g_cmp.test_count; ++i) {
        if (!used[i]) continue;
        remap[i] = (u16)t;
        if (t != i) g_cmp.tests[t] = g_cmp.tests[i];
        ++t;
    }
    g_cmp.test_count = t;
    for (u32 i = 0; i < g_cmp.row_count; ++i) g_cmp.rows[i].test = remap[g_cmp.rows[i].test];
}

void compare_remove(u32 run) {
    if (run >= g_cmp.run_count) return;
    const u32 last = g_cmp.run_count - 1;
    for (u32 c = run; c < last; ++c) g_cmp.runs[c] = g_cmp.runs[c + 1];
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        for (u32 c = run; c < last; ++c) g_cmp.values[i][c] = g_cmp.values[i][c + 1];
        g_cmp.values[i][last] = NAN;
    }
    memset(&g_cmp.runs[last], 0, sizeof(CmpRun));
    g_cmp.run_count = last;
    if (g_cmp.reference == run) g_cmp.reference = 0;
    else if (g_cmp.reference > run) g_cmp.reference--;
    compact();
}

// ---- summary ------------------------------------------------------------------------------------

static bool in_summary(const CmpRow* r) {
    if (r->stat == CS_MIN || r->stat == CS_MAX) return false;
    if (!strcmp(r->unit, "x") || !strcmp(r->unit, "%")) return false;
    // p99.9 frame times rest on a handful of frames at these frame rates; one hitch moves them 2-8x and would
    // dominate the averages.
    if (strstr(r->name, "p99.9")) return false;
    return strncmp(r->name, "Theoretical", 11) != 0;
}

bool compare_summary(u32 run, const char* suite, CmpSummary* out) {
    memset(out, 0, sizeof(*out));
    const u32 ref = g_cmp.reference;
    f64 log_sum = 0.0;
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        const CmpRow* r = &g_cmp.rows[i];
        if (!in_summary(r)) continue;
        if (suite && strcmp(g_cmp.tests[r->test].suite, suite) != 0) continue;
        const f64 v = g_cmp.values[i][run], base = g_cmp.values[i][ref];
        if (!(v > 0.0) || !(base > 0.0)) continue;      // also NaN (missing)
        const f64 ratio = unit_lower_is_better(r->unit) ? base / v : v / base;
        const f64 pct = (ratio - 1.0) * 100.0;
        log_sum += log(ratio);
        if (out->compared == 0 || pct > out->best_pct) { out->best_pct = pct; out->best_row = i; }
        if (out->compared == 0 || pct < out->worst_pct) { out->worst_pct = pct; out->worst_row = i; }
        out->compared++;
        if (pct >= 1.0) out->better++;
        else if (pct <= -1.0) out->worse++;
        else out->same++;
    }
    if (out->compared) out->geomean_pct = (exp(log_sum / out->compared) - 1.0) * 100.0;
    return out->compared > 0;
}

// ---- system info strings --------------------------------------------------------------------------

static void format_ram(char* out, u32 size, f64 installed_bytes, f64 total_bytes, const char* type, u32 mts) {
    const f64 bytes = installed_bytes > 0 ? installed_bytes : total_bytes;
    if (type && type[0] && mts) snprintf(out, size, "%.0f GiB %s-%u", bytes / GiB, type, mts);
    else snprintf(out, size, "%.1f GiB", bytes / GiB);
}

static void format_power(char* out, u32 size, bool on_ac, const char* mode) {
    snprintf(out, size, "%s%s%s", on_ac ? "AC" : "Battery", mode && mode[0] ? ", " : "", mode ? mode : "");
}

// ---- loading a TOON results file --------------------------------------------------------------------

struct FileTest {
    char id[48];
    u32 cmp_test;               // index into g_cmp.tests
    bool matrix;
    u32 rows, cols;
    char matrix_name[64], matrix_unit[16];
    f64 lo, hi, sum;            // off-diagonal accumulators
    u32 n;
};

struct FileState {
    FileTest tests[CMP_MAX_TESTS];
    u32 test_count;
    char timestamp[32], cpu[96], gpu[96], os[64], plan[64], mode[64], ram_type[16];
    char drives[26][64];        // model per drive letter
    u32 disk_mask;
    f64 ram_installed, ram_total, duration_ms;
    u32 ram_mts;
    bool on_ac, elevated, have_app;
};
static FileState s_file;

static i32 field(const ToonLine* l, const char* name) {
    for (u32 i = 0; i < l->field_count; ++i)
        if (!strcmp(l->fields[i], name)) return (i32)i;
    return -1;
}

static const char* cell(char cells[][TOON_CELL], i32 i) { return i >= 0 ? cells[i] : ""; }

static bool read_table(ToonReader* r, const ToonLine* l, const char* path, u32 run, char* err, u32 err_size) {
    static char cells[TOON_MAX_FIELDS][TOON_CELL];
    const bool info_only = run == UINT32_MAX;       // compare_peek: system info only
    const bool tests = !info_only && !strcmp(path, "tests"), metrics = !info_only && !strcmp(path, "metrics");
    const bool modules = !strcmp(path, "system.ram.modules"), drives = !strcmp(path, "system.drives");
    const char *s, *e;
    u32 indent;
    for (u32 row = 0; row < l->count; ++row) {
        if (!toon_next_line(r, &s, &e, &indent)) { snprintf(err, err_size, "%s table ends early", l->key); return false; }
        if (!tests && !metrics && !modules && !drives) continue;     // tables we don't need
        const u32 n = toon_split(s, e, cells, TOON_MAX_FIELDS);
        if (n != l->field_count) { snprintf(err, err_size, "line %u: %u cells, expected %u", r->line, n, l->field_count); return false; }

        if (modules && row == 0) {      // first module describes the kit
            copy_str(s_file.ram_type, sizeof(s_file.ram_type), cell(cells, field(l, "type")));
            s_file.ram_mts = (u32)atoi(cell(cells, field(l, "configured_mts")));
        } else if (drives) {
            const char* letter = cell(cells, field(l, "letter"));
            if (letter[0] >= 'A' && letter[0] <= 'Z')
                copy_str(s_file.drives[letter[0] - 'A'], sizeof(s_file.drives[0]), cell(cells, field(l, "model")));
        } else if (tests) {
            if (s_file.test_count >= CMP_MAX_TESTS) continue;
            const char* id = cell(cells, field(l, "id"));
            char title[96];
            snprintf(title, sizeof(title), "%s / %s", cell(cells, field(l, "suite")), cell(cells, field(l, "name")));
            FileTest* t = &s_file.tests[s_file.test_count++];
            memset(t, 0, sizeof(*t));
            copy_str(t->id, sizeof(t->id), id);
            t->cmp_test = find_test(id, cell(cells, field(l, "suite")), title, cell(cells, field(l, "x_unit")));
            t->matrix = !strcmp(cell(cells, field(l, "kind")), "matrix");
            t->rows = (u32)atoi(cell(cells, field(l, "rows")));
            t->cols = (u32)atoi(cell(cells, field(l, "cols")));
        } else {
            const char* id = cell(cells, field(l, "id"));
            FileTest* t = nullptr;
            for (u32 i = 0; i < s_file.test_count && !t; ++i)
                if (!strcmp(s_file.tests[i].id, id)) t = &s_file.tests[i];
            const char* vs = cell(cells, field(l, "value"));
            char* stop = nullptr;
            const f64 value = strtod(vs, &stop);
            if (stop == vs) continue;   // null
            const f64 x = strtod(cell(cells, field(l, "x")), nullptr);
            const char* name = cell(cells, field(l, "name"));
            const char* unit = cell(cells, field(l, "unit"));
            if (!t) {   // metric of a test missing from the tests table: show it under its id
                add_value(run, find_test(id, "", id, ""), CS_VALUE, name, unit, x, value);
            } else if (t->matrix) {
                const u32 k = (u32)x;
                if (t->rows == t->cols && t->cols && k / t->cols == k % t->cols) continue;      // diagonal
                if (!t->n) {
                    copy_str(t->matrix_name, sizeof(t->matrix_name), name);
                    copy_str(t->matrix_unit, sizeof(t->matrix_unit), unit);
                }
                t->lo = t->n == 0 || value < t->lo ? value : t->lo;
                t->hi = t->n == 0 || value > t->hi ? value : t->hi;
                t->sum += value;
                t->n++;
            } else {
                add_value(run, t->cmp_test, CS_VALUE, name, unit, x, value);
            }
        }
    }
    if (metrics)
        for (u32 i = 0; i < s_file.test_count; ++i) {
            const FileTest* t = &s_file.tests[i];
            if (t->matrix && t->n) add_matrix(run, t->cmp_test, t->matrix_name, t->matrix_unit, t->lo, t->sum / t->n, t->hi);
        }
    return true;
}

static bool parse_file(ToonReader* r, u32 run, char* err, u32 err_size) {
    char keys[8][64] = {};      // key path by depth
    const char *s, *e;
    u32 indent;
    while (toon_next_line(r, &s, &e, &indent)) {
        const u32 depth = indent / 2;
        ToonLine l;
        if (depth >= 8 || !toon_parse_key(s, e, &l)) { snprintf(err, err_size, "line %u: expected 'key:'", r->line); return false; }
        copy_str(keys[depth], sizeof(keys[0]), l.key);
        char path[256] = {};
        for (u32 d = 0; d <= depth; ++d) {
            if (d) strcat_s(path, ".");
            strcat_s(path, keys[d]);
        }
        char v[TOON_CELL];
        toon_decode(l.value, l.value_end, v, sizeof(v));
        if (l.array && l.field_count) {
            if (!read_table(r, &l, path, run, err, err_size)) return false;
        } else if (!strcmp(path, "params.disk_drives")) {
            for (const char* p = v; *p; ++p)
                if (*p >= 'A' && *p <= 'Z') s_file.disk_mask |= 1u << (*p - 'A');
        }
        else if (!strcmp(path, "app")) s_file.have_app = !strcmp(v, "ez-pc-bench");
        else if (!strcmp(path, "kind") && strcmp(v, "benchmark") != 0) {     // e.g. a stability report
            snprintf(err, err_size, "a %s report, not benchmark results", v);
            return false;
        }
        else if (!strcmp(path, "timestamp")) copy_str(s_file.timestamp, sizeof(s_file.timestamp), v);
        else if (!strcmp(path, "system.cpu.brand")) copy_str(s_file.cpu, sizeof(s_file.cpu), v);
        else if (!strcmp(path, "system.ram.installed_bytes")) s_file.ram_installed = atof(v);
        else if (!strcmp(path, "system.ram.total_bytes")) s_file.ram_total = atof(v);
        else if (!strcmp(path, "system.os")) copy_str(s_file.os, sizeof(s_file.os), v);
        else if (!strcmp(path, "system.elevated")) s_file.elevated = !strcmp(v, "true");
        else if (!strcmp(path, "system.power.on_ac")) s_file.on_ac = !strcmp(v, "true");
        else if (!strcmp(path, "system.power.mode")) copy_str(s_file.mode, sizeof(s_file.mode), v);
        else if (!strcmp(path, "params.gpu")) copy_str(s_file.gpu, sizeof(s_file.gpu), v);
        else if (!strcmp(path, "params.duration_ms")) s_file.duration_ms = atof(v);
    }
    if (!s_file.have_app) { snprintf(err, err_size, "not an EzPcBench results file"); return false; }
    return true;
}

static void set_disk_info(CmpRun* run, u32 mask, const char (*models)[64]) {
    char* out = run->info[CI_DISK];
    const u32 size = sizeof(run->info[0]);
    out[0] = 0;
    for (u32 d = 0, n = 0; d < 26; ++d) {
        if (!((mask >> d) & 1)) continue;
        const size_t len = strlen(out);
        snprintf(out + len, size - len, "%s%c: %s", n++ ? "; " : "", 'A' + d, models[d]);
    }
}

static u32 begin_run(char* err, u32 err_size) {
    if (g_cmp.run_count >= CMP_MAX_RUNS) { snprintf(err, err_size, "at most %u runs", CMP_MAX_RUNS); return UINT32_MAX; }
    const u32 run = g_cmp.run_count++;
    memset(&g_cmp.runs[run], 0, sizeof(CmpRun));
    return run;
}

static void fill_info(CmpRun* c) {
    copy_str(c->info[CI_DATE], sizeof(c->info[0]), s_file.timestamp);
    if (char* t = strchr(c->info[CI_DATE], 'T')) *t = ' ';
    copy_str(c->info[CI_CPU], sizeof(c->info[0]), s_file.cpu);
    format_ram(c->info[CI_RAM], sizeof(c->info[0]), s_file.ram_installed, s_file.ram_total, s_file.ram_type, s_file.ram_mts);
    copy_str(c->info[CI_GPU], sizeof(c->info[0]), s_file.gpu);
    set_disk_info(c, s_file.disk_mask, s_file.drives);
    copy_str(c->info[CI_OS], sizeof(c->info[0]), s_file.os);
    format_power(c->info[CI_POWER], sizeof(c->info[0]), s_file.on_ac, s_file.mode);
    copy_str(c->info[CI_ADMIN], sizeof(c->info[0]), s_file.elevated ? "yes" : "no");
    snprintf(c->info[CI_DURATION], sizeof(c->info[0]), "%g s", s_file.duration_ms / 1000.0);
}

bool compare_add_buffer(const char* label, const char* data, u32 size, char* err, u32 err_size) {
    const u32 run = begin_run(err, err_size);
    if (run == UINT32_MAX) return false;
    ToonReader r = { data, data + size, 0 };
    if (size >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) r.p += 3;
    memset(&s_file, 0, sizeof(s_file));
    if (!parse_file(&r, run, err, err_size)) {
        compare_remove(run);
        return false;
    }
    if (g_cmp.runs[run].metrics == 0) {     // e.g. a run cancelled before its first test finished
        compare_remove(run);
        snprintf(err, err_size, "no finished tests in this run");
        return false;
    }
    CmpRun* c = &g_cmp.runs[run];
    copy_str(c->label, sizeof(c->label), label);
    fill_info(c);
    return true;
}

bool compare_add_file(const wchar_t* path, char* err, u32 err_size) {
    ToonReader r;
    char* buf = toon_read_file(path, &r, err, err_size);
    if (!buf) return false;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    char label[96];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, label, sizeof(label), nullptr, nullptr);
    if (char* ext = strstr(label, ".toon.txt")) *ext = 0;
    const bool ok = compare_add_buffer(label, r.p, (u32)(r.end - r.p), err, err_size);
    toon_free_file(buf);
    return ok;
}

bool compare_peek(const char* data, u32 size, CmpRun* out, char* err, u32 err_size) {
    memset(out, 0, sizeof(*out));
    ToonReader r = { data, data + size, 0 };
    if (size >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) r.p += 3;
    memset(&s_file, 0, sizeof(s_file));
    if (!parse_file(&r, UINT32_MAX, err, err_size)) return false;
    fill_info(out);
    return true;
}

bool compare_has_label(const char* label) {
    for (u32 c = 0; c < g_cmp.run_count; ++c)
        if (!strcmp(g_cmp.runs[c].label, label)) return true;
    return false;
}

// ---- the current run ------------------------------------------------------------------------------

bool compare_add_current(char* err, u32 err_size) {
    if (runner_is_running()) { snprintf(err, err_size, "a run is in progress"); return false; }
    u32 done = 0;
    for (u32 i = 0; i < g_bench_count; ++i) done += g_status[i].load(std::memory_order_acquire) == ST_DONE;
    if (!done) { snprintf(err, err_size, "no finished tests in this session"); return false; }
    const u32 run = begin_run(err, err_size);
    if (run == UINT32_MAX) return false;

    for (u32 i = 0; i < g_bench_count; ++i) {
        if (g_status[i].load(std::memory_order_acquire) != ST_DONE) continue;
        const BenchDesc* b = &g_benches[i];
        const BenchResult* r = &g_results[i];
        char title[96];
        snprintf(title, sizeof(title), "%s / %s", g_suites[b->suite].name, b->name);
        const u32 test = find_test(b->cli_id, g_suites[b->suite].name, title, r->x_unit);
        const Metric* m = res_metrics(r);
        if (r->kind == RESULT_MATRIX) {
            if (!r->count) continue;
            f64 lo, avg, hi;
            res_matrix_stats(r, &lo, &avg, &hi);
            add_matrix(run, test, m[0].name, m[0].unit, lo, avg, hi);
            continue;
        }
        for (u32 k = 0; k < r->count; ++k)
            add_value(run, test, CS_VALUE, m[k].name, m[k].unit, r->kind == RESULT_SERIES ? m[k].x : 0.0, m[k].value);
    }

    const SysInfo* s = &g_sys;
    const BenchParams* p = &g_run.params;
    CmpRun* c = &g_cmp.runs[run];
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    strftime(c->label, sizeof(c->label), "Current run (%H:%M)", &t);
    strftime(c->info[CI_DATE], sizeof(c->info[0]), "%Y-%m-%d %H:%M:%S", &t);
    copy_str(c->info[CI_CPU], sizeof(c->info[0]), s->cpu.brand);
    const RamModule* m0 = s->ram.module_count ? &s->ram.modules[0] : nullptr;
    format_ram(c->info[CI_RAM], sizeof(c->info[0]), (f64)s->ram.installed_bytes, (f64)s->ram.total_bytes, m0 ? m0->type : "",
               m0 ? m0->configured_mts : 0);
    copy_str(c->info[CI_GPU], sizeof(c->info[0]), p->gpu_adapter < s->gpu_count ? s->gpus[p->gpu_adapter].name : "");
    char models[26][64] = {};
    for (u32 i = 0; i < s->drive_count; ++i)
        if (s->drives[i].letter >= 'A' && s->drives[i].letter <= 'Z')
            copy_str(models[s->drives[i].letter - 'A'], sizeof(models[0]), s->drives[i].model);
    set_disk_info(c, p->disk_drives, models);
    copy_str(c->info[CI_OS], sizeof(c->info[0]), s->os);
    format_power(c->info[CI_POWER], sizeof(c->info[0]), s->power.on_ac, power_mode_name(s->power.mode));
    copy_str(c->info[CI_ADMIN], sizeof(c->info[0]), s->elevated ? "yes" : "no");
    snprintf(c->info[CI_DURATION], sizeof(c->info[0]), "%g s", p->duration_ms / 1000.0);
    return true;
}
