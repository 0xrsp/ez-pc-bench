#include "core/results.h"
#include "core/baseline.h"
#include "core/integrity.h"
#include "core/monitor.h"
#include "core/runner.h"
#include "core/score.h"
#include "core/sysinfo.h"
#include "core/toon.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

Metric g_metrics[MAX_METRICS];
MetricSpread g_spread[MAX_METRICS];
BenchResult g_results[MAX_BENCHES];
std::atomic<u8> g_status[MAX_BENCHES];
static u32 s_metric_count;

void results_reset() {
    memset(g_spread, 0, s_metric_count * sizeof(g_spread[0]));
    s_metric_count = 0;
    memset(g_results, 0, sizeof(g_results));
    monitor_reset();
}

u32 res_mark() { return s_metric_count; }
void res_rewind(u32 mark) { if (mark <= s_metric_count) s_metric_count = mark; }

void res_begin(BenchResult* r, ResultKind kind) {
    r->first = s_metric_count;
    r->count = 0;
    r->kind = kind;
}

void res_add(BenchResult* r, const char* name, const char* unit, f64 value, f64 x) {
    if (s_metric_count >= MAX_METRICS) return;
    g_metrics[s_metric_count++] = { name, unit, value, x };
    r->count++;
}

void res_matrix(BenchResult* r, u16 rows, u16 cols) {
    r->kind = RESULT_MATRIX;
    r->rows = rows;
    r->cols = cols;
}

void res_series_axis(BenchResult* r, const char* x_name, const char* x_unit) {
    r->x_name = x_name;
    r->x_unit = x_unit;
}

void res_fail(BenchResult* r, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, args);
    va_end(args);
    if (!r->error[0]) strcpy(r->error, "failed");
}

void res_skip(BenchResult* r, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, args);
    va_end(args);
    r->skipped = true;
}

void res_matrix_stats(const BenchResult* r, f64* lo, f64* avg, f64* hi) {
    const Metric* m = res_metrics(r);
    f64 sum = 0.0;
    u32 n = 0;
    *lo = *avg = *hi = 0.0;
    for (u32 k = 0; k < r->count; ++k) {
        if (r->rows == r->cols && r->cols && k / r->cols == k % r->cols) continue;
        const f64 v = m[k].value;
        *lo = n == 0 || v < *lo ? v : *lo;
        *hi = n == 0 || v > *hi ? v : *hi;
        sum += v;
        ++n;
    }
    if (n) *avg = sum / n;
}

const char* status_name(BenchStatus s) {
    switch (s) {
        case ST_PENDING: return "pending";
        case ST_RUNNING: return "running";
        case ST_DONE: return "done";
        case ST_SKIPPED: return "skipped";
        case ST_FAILED: return "failed";
        case ST_CANCELLED: return "cancelled";
        default: return "not run";
    }
}

void format_value(char* buf, u32 size, f64 v) {
    f64 a = fabs(v);
    if (a == 0.0) snprintf(buf, size, "0");
    else if (a >= 1000.0) snprintf(buf, size, "%.0f", v);
    else if (a >= 100.0) snprintf(buf, size, "%.1f", v);
    else if (a >= 1.0) snprintf(buf, size, "%.2f", v);
    else snprintf(buf, size, "%.3g", v);
}

void format_x(char* buf, u32 size, f64 x, const char* x_unit) {
    if (x_unit && strcmp(x_unit, "bytes") == 0) {
        u64 b = (u64)x;
        if (b >= GiB && b % GiB == 0) snprintf(buf, size, "%llu GiB", b / GiB);
        else if (b >= MiB && b % MiB == 0) snprintf(buf, size, "%llu MiB", b / MiB);
        else if (b >= KiB && b % KiB == 0) snprintf(buf, size, "%llu KiB", b / KiB);
        else snprintf(buf, size, "%llu B", b);
    } else {
        snprintf(buf, size, "%g %s", x, x_unit ? x_unit : "");
    }
}

bool format_frame_fps(char* buf, u32 size, const char* name, const char* unit, f64 value) {
    buf[0] = 0;
    if (!name || !unit || strcmp(unit, "ms") != 0 || !strstr(name, "frame time") || !(value > 0.0)) return false;
    char v[32];
    format_value(v, sizeof(v), 1000.0 / value);
    snprintf(buf, size, " (%s fps)", v);
    return true;
}

// ---- TOON export -------------------------------------------------------------
// Layout: app / format / timestamp, `system:` and `params:` objects, then two flat tables:
// tests[N]{id,suite,group,name,status,seconds,kind,rows,cols,x_name,x_unit,error} and
// metrics[M]{id,name,unit,value,x}. x = series x, matrix cell index, 0 for plain values
// (the key the baseline uses, see baseline_key_x).

static const char* kind_name(ResultKind k) {
    return k == RESULT_SERIES ? "series" : k == RESULT_MATRIX ? "matrix" : "values";
}

static void write_system(FILE* f) {
    const SysInfo* s = &g_sys;
    toon_key(f, 0, "system");
    toon_key(f, 1, "cpu");
    toon_str(f, 2, "brand", s->cpu.brand);
    toon_str(f, 2, "vendor", s->cpu.vendor);
    toon_num(f, 2, "family", s->cpu.family);
    toon_num(f, 2, "model", s->cpu.model);
    toon_num(f, 2, "stepping", s->cpu.stepping);
    toon_num(f, 2, "cores", s->cpu.cores);
    toon_num(f, 2, "threads", s->cpu.threads);
    toon_num(f, 2, "l1d_kib", s->cpu.l1d_kib);
    toon_num(f, 2, "l2_kib", s->cpu.l2_kib);
    toon_num(f, 2, "l3_kib", s->cpu.l3_kib);
    toon_bool(f, 2, "avx2", s->cpu.avx2);
    toon_bool(f, 2, "avx512f", s->cpu.avx512f);

    toon_key(f, 1, "ram");
    toon_num(f, 2, "total_bytes", (f64)s->ram.total_bytes);
    toon_num(f, 2, "installed_bytes", (f64)s->ram.installed_bytes);
    toon_table(f, 2, "modules", s->ram.module_count, "type,size_bytes,speed_mts,configured_mts,manufacturer,part,locator");
    for (u32 i = 0; i < s->ram.module_count; ++i) {
        const RamModule* m = &s->ram.modules[i];
        ToonRow r = toon_row(f, 3);
        toon_cell_str(&r, m->type);
        toon_cell_num(&r, (f64)m->size_bytes);
        toon_cell_num(&r, m->speed_mts);
        toon_cell_num(&r, m->configured_mts);
        toon_cell_str(&r, m->manufacturer);
        toon_cell_str(&r, m->part);
        toon_cell_str(&r, m->locator);
        toon_row_end(&r);
    }

    toon_table(f, 1, "gpus", s->gpu_count, "name,dedicated_vram,vendor_id,device_id,driver,hags");
    for (u32 i = 0; i < s->gpu_count; ++i) {
        const GpuInfo* g = &s->gpus[i];
        ToonRow r = toon_row(f, 2);
        toon_cell_str(&r, g->name);
        toon_cell_num(&r, (f64)g->dedicated_vram);
        toon_cell_num(&r, g->vendor_id);
        toon_cell_num(&r, g->device_id);
        toon_cell_str(&r, g->driver);
        toon_cell_str(&r, g->hags < 0 ? "unsupported" : g->hags ? "on" : "off");
        toon_row_end(&r);
    }

    toon_key(f, 1, "board");
    toon_str(f, 2, "system", s->board.sys_vendor);
    toon_str(f, 2, "product", s->board.sys_product);
    toon_str(f, 2, "board_vendor", s->board.board_vendor);
    toon_str(f, 2, "board", s->board.board_product);
    toon_str(f, 2, "bios_vendor", s->board.bios_vendor);
    toon_str(f, 2, "bios_version", s->board.bios_version);
    toon_str(f, 2, "bios_date", s->board.bios_date);

    toon_table(f, 1, "drives", s->drive_count,
               "letter,model,bus,fs,total_bytes,logical_sector,physical_sector,virtual_disk,dev_drive,bitlocker");
    for (u32 i = 0; i < s->drive_count; ++i) {
        const DriveInfo* d = &s->drives[i];
        const char letter[2] = { d->letter, 0 };
        ToonRow r = toon_row(f, 2);
        toon_cell_str(&r, letter);
        toon_cell_str(&r, d->model);
        toon_cell_str(&r, d->bus);
        toon_cell_str(&r, d->fs);
        toon_cell_num(&r, (f64)d->total_bytes);
        toon_cell_num(&r, d->logical_sector);
        toon_cell_num(&r, d->physical_sector);
        toon_cell_bool(&r, d->virtual_disk);
        toon_cell_bool(&r, d->dev_drive);
        toon_cell_bool(&r, d->bitlocker_on);
        toon_row_end(&r);
    }

    toon_str(f, 1, "os", s->os);
    toon_bool(f, 1, "elevated", s->elevated);
    toon_key(f, 1, "windows");
    const i8 w[] = { s->win.hypervisor, s->win.vbs, s->win.hvci, s->win.secure_boot, s->win.game_mode };
    const char* const wn[] = { "hypervisor", "vbs", "memory_integrity", "secure_boot", "game_mode" };
    for (u32 i = 0; i < ARRAY_COUNT(w); ++i) toon_str(f, 2, wn[i], w[i] < 0 ? "unknown" : w[i] ? "on" : "off");
    toon_str(f, 2, "tpm", s->win.tpm == 20 ? "2.0" : s->win.tpm == 12 ? "1.2" : "none");
    toon_key(f, 1, "power");
    toon_bool(f, 2, "on_ac", s->power.on_ac);
    toon_str(f, 2, "plan", s->power.scheme_name);
    toon_str(f, 2, "mode", power_mode_name(s->power.mode));
}

static void write_params(FILE* f, const BenchParams* p) {
    toon_key(f, 0, "params");
    toon_num(f, 1, "duration_ms", p->duration_ms);
    toon_num(f, 1, "thread_count", p->thread_count);
    toon_str(f, 1, "gpu", p->gpu_adapter < g_sys.gpu_count ? g_sys.gpus[p->gpu_adapter].name : "");
    toon_num(f, 1, "gpu_width", p->gpu_width);
    toon_num(f, 1, "gpu_height", p->gpu_height);
    u32 drives = 0;
    for (u32 d = 0; d < 26; ++d) drives += (p->disk_drives >> d) & 1;
    fprintf(f, "  disk_drives[%u]: ", drives);
    for (u32 d = 0, n = 0; d < 26; ++d)
        if ((p->disk_drives >> d) & 1) fprintf(f, "%s%c", n++ ? "," : "", 'A' + d);
    fputc('\n', f);
    toon_num(f, 1, "disk_file_bytes", (f64)p->disk_file_bytes);
    toon_num(f, 1, "disk_passes", p->disk_passes);
    toon_bool(f, 1, "disk_write_through", p->disk_write_through);
    toon_num(f, 1, "repeat", p->repeat);
}

// Repeated runs: spread[N]{id,name,x,runs,min,max,cv} for every metric whose value is a median.
static void write_spread(FILE* f) {
    u32 n = 0;
    for (u32 i = 0; i < g_bench_count; ++i)
        if ((BenchStatus)g_status[i].load(std::memory_order_acquire) == ST_DONE && g_results[i].count &&
            res_spread(&g_results[i])->runs >= 2)
            n += g_results[i].count;
    if (!n) return;
    toon_table(f, 0, "spread", n, "id,name,x,runs,min,max,cv");
    for (u32 i = 0; i < g_bench_count; ++i) {
        const BenchResult* r = &g_results[i];
        if ((BenchStatus)g_status[i].load(std::memory_order_acquire) != ST_DONE || !r->count || res_spread(r)->runs < 2)
            continue;
        const Metric* m = res_metrics(r);
        const MetricSpread* sp = res_spread(r);
        for (u32 k = 0; k < r->count; ++k) {
            ToonRow row = toon_row(f, 1);
            toon_cell_str(&row, g_benches[i].cli_id);
            toon_cell_str(&row, m[k].name);
            toon_cell_num(&row, baseline_key_x(r, k));
            toon_cell_num(&row, sp[k].runs);
            toon_cell_num(&row, sp[k].min);
            toon_cell_num(&row, sp[k].max);
            toon_cell_num(&row, round(sp[k].cv * 100.0) / 100.0);
            toon_row_end(&row);
        }
    }
}

// Conditions each test ran under (monitor.h): env[N]{id,samples,cpu_mhz,...,flags}. NaN = not reported.
static void write_env(FILE* f) {
    u32 n = 0;
    for (u32 i = 0; i < g_bench_count; ++i) n += env_valid(&g_env[i]);
    if (!n) return;
    toon_table(f, 0, "env", n, "id,samples,cpu_mhz,cpu_mhz_max,cpu_util,cpu_temp_max,gpu_mhz,gpu_mhz_max,gpu_mem_mhz,"
                               "gpu_util,gpu_temp_max,gpu_power,cpu_drop,gpu_drop,flags");
    for (u32 i = 0; i < g_bench_count; ++i) {
        const TestEnv* e = &g_env[i];
        if (!env_valid(e)) continue;
        char flags[96];
        env_flag_names(e->flags, flags, sizeof(flags));
        ToonRow row = toon_row(f, 1);
        toon_cell_str(&row, g_benches[i].cli_id);
        toon_cell_num(&row, e->samples);
        const f32 v[] = { e->cpu_mhz, e->cpu_mhz_max, e->cpu_util, e->cpu_temp_max, e->gpu_mhz, e->gpu_mhz_max,
                          e->gpu_mem_mhz, e->gpu_util, e->gpu_temp_max, e->gpu_power, e->cpu_drop, e->gpu_drop };
        for (u32 k = 0; k < ARRAY_COUNT(v); ++k) toon_cell_num(&row, round(v[k] * 10.0) / 10.0);
        toon_cell_str(&row, flags);
        toon_row_end(&row);
    }
}

bool results_write_toon(const wchar_t* path, const BenchParams* p) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;

    char stamp[32];
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_s(&tm_local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
    toon_str(f, 0, "app", "ez-pc-bench");
    toon_num(f, 0, "format", 1);
    toon_str(f, 0, "timestamp", stamp);
    write_system(f);
    write_params(f, p);

    u32 tests = 0, metrics = 0;
    for (u32 i = 0; i < g_bench_count; ++i) {
        const BenchStatus st = (BenchStatus)g_status[i].load(std::memory_order_acquire);
        if (st == ST_NOT_RUN) continue;
        ++tests;
        if (st == ST_DONE) metrics += g_results[i].count;
    }

    toon_table(f, 0, "tests", tests, "id,suite,group,name,status,seconds,kind,rows,cols,x_name,x_unit,error");
    for (u32 i = 0; i < g_bench_count; ++i) {
        const BenchStatus st = (BenchStatus)g_status[i].load(std::memory_order_acquire);
        if (st == ST_NOT_RUN) continue;
        const BenchDesc* b = &g_benches[i];
        const BenchResult* r = &g_results[i];
        ToonRow row = toon_row(f, 1);
        toon_cell_str(&row, b->cli_id);
        toon_cell_str(&row, g_suites[b->suite].name);
        toon_cell_str(&row, b->group);
        toon_cell_str(&row, b->name);
        toon_cell_str(&row, status_name(st));
        toon_cell_num(&row, r->seconds);
        toon_cell_str(&row, kind_name(r->kind));
        toon_cell_num(&row, r->rows);
        toon_cell_num(&row, r->cols);
        toon_cell_str(&row, r->x_name ? r->x_name : "");
        toon_cell_str(&row, r->x_unit ? r->x_unit : "");
        toon_cell_str(&row, r->error);
        toon_row_end(&row);
    }

    toon_table(f, 0, "metrics", metrics, "id,name,unit,value,x");
    for (u32 i = 0; i < g_bench_count; ++i) {
        if ((BenchStatus)g_status[i].load(std::memory_order_acquire) != ST_DONE) continue;
        const BenchResult* r = &g_results[i];
        const Metric* m = res_metrics(r);
        for (u32 k = 0; k < r->count; ++k) {
            ToonRow row = toon_row(f, 1);
            toon_cell_str(&row, g_benches[i].cli_id);
            toon_cell_str(&row, m[k].name);
            toon_cell_str(&row, m[k].unit);
            toon_cell_num(&row, m[k].value);
            toon_cell_num(&row, baseline_key_x(r, k));
            toon_row_end(&row);
        }
    }
    write_spread(f);
    write_env(f);
    ScoreResult score;      // derived values last, then the integrity hashes (integrity.h)
    score_current(&score);
    score_write_toon(f, &score);
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok && integrity_append(path);
}
