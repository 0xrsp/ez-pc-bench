#include "core/results.h"
#include "core/baseline.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/toon.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

Metric g_metrics[MAX_METRICS];
BenchResult g_results[MAX_BENCHES];
std::atomic<u8> g_status[MAX_BENCHES];
static u32 s_metric_count;

void results_reset() {
    s_metric_count = 0;
    memset(g_results, 0, sizeof(g_results));
}

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

// ---- text --------------------------------------------------------------------

struct TextBuf { char* p; u32 size, len; };

static void tb_printf(TextBuf* tb, const char* fmt, ...) {
    if (tb->len + 1 >= tb->size) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tb->p + tb->len, tb->size - tb->len, fmt, args);
    va_end(args);
    if (n > 0) tb->len = (tb->len + (u32)n < tb->size) ? tb->len + (u32)n : tb->size - 1;
}

u32 results_format_text(char* buf, u32 size, const BenchParams* p) {
    TextBuf tb = { buf, size, 0 };
    if (size) buf[0] = 0;
    const SysInfo* s = &g_sys;
    tb_printf(&tb, "ez-pc-bench results\n");
    tb_printf(&tb, "CPU:   %s (%uC/%uT, L1D %u KiB, L2 %u KiB, L3 %u KiB)\n", s->cpu.brand, s->cpu.cores,
              s->cpu.threads, s->cpu.l1d_kib, s->cpu.l2_kib, s->cpu.l3_kib);
    tb_printf(&tb, "RAM:   %.1f GiB usable", s->ram.total_bytes / (f64)GiB);
    for (u32 i = 0; i < s->ram.module_count; ++i) {
        const RamModule* m = &s->ram.modules[i];
        tb_printf(&tb, "%s %llu GiB %s-%u", i ? "," : " |", m->size_bytes / GiB, m->type, m->configured_mts);
    }
    tb_printf(&tb, "\n");
    for (u32 i = 0; i < s->gpu_count; ++i)
        tb_printf(&tb, "GPU %u: %s (%llu MiB VRAM)%s\n", i, s->gpus[i].name, s->gpus[i].dedicated_vram / MiB,
                  i == p->gpu_adapter ? "  [selected]" : "");
    for (u32 i = 0; i < s->drive_count; ++i) {
        char d[192];
        drive_describe(&s->drives[i], d, sizeof(d));
        tb_printf(&tb, "Disk %c: %s%s\n", s->drives[i].letter, d, ((p->disk_drives >> (s->drives[i].letter - 'A')) & 1) ? "  [target]" : "");
    }
    tb_printf(&tb, "OS:    %s, %s\n", s->os, s->elevated ? "elevated" : "not elevated");
    tb_printf(&tb, "Power: %s, plan \"%s\", mode \"%s\"\n", s->power.on_ac ? "AC" : "battery", s->power.scheme_name,
              power_mode_name(s->power.mode));
    if (g_baseline.loaded) tb_printf(&tb, "Baseline: %s (%s), change in %%\n", g_baseline.file, g_baseline.timestamp);
    tb_printf(&tb, "\n");

    for (u32 i = 0; i < g_bench_count; ++i) {
        BenchStatus st = (BenchStatus)g_status[i].load(std::memory_order_acquire);
        if (st == ST_NOT_RUN) continue;
        const BenchDesc* b = &g_benches[i];
        const BenchResult* r = &g_results[i];
        tb_printf(&tb, "[%s] %s / %s / %s", status_name(st), g_suites[b->suite].name, b->group, b->name);
        if (r->error[0]) tb_printf(&tb, "  (%s)", r->error);
        tb_printf(&tb, "\n");
        if (st != ST_DONE) continue;
        const Metric* m = res_metrics(r);
        char v[32], x[32];
        if (r->kind == RESULT_MATRIX) {
            tb_printf(&tb, "    %s [%s], %ux%u:\n", m[0].name, m[0].unit, r->rows, r->cols);
            for (u32 row = 0; row < r->rows; ++row) {
                tb_printf(&tb, "     ");
                for (u32 c = 0; c < r->cols && row * r->cols + c < r->count; ++c) {
                    format_value(v, sizeof(v), m[row * r->cols + c].value);
                    tb_printf(&tb, " %9s", v);
                }
                tb_printf(&tb, "\n");
            }
            f64 lo, avg, hi, blo, bavg, bhi;
            if (baseline_matrix(b->cli_id, r, &blo, &bavg, &bhi)) {
                res_matrix_stats(r, &lo, &avg, &hi);
                char bv[32], d[16];
                format_value(v, sizeof(v), avg);
                format_value(bv, sizeof(bv), bavg);
                format_delta(d, sizeof(d), delta_pct(avg, bavg));
                tb_printf(&tb, "    average %s, baseline %s, %s\n", v, bv, d);
            }
        } else {
            const int uw = g_baseline.loaded ? 8 : 0;    // pad the unit only when a baseline column follows
            for (u32 k = 0; k < r->count; ++k) {
                format_value(v, sizeof(v), m[k].value);
                if (r->kind == RESULT_SERIES) {
                    format_x(x, sizeof(x), m[k].x, r->x_unit);
                    tb_printf(&tb, "    %-12s %-20s %12s %-*s", x, m[k].name, v, uw, m[k].unit);
                } else {
                    tb_printf(&tb, "    %-33s %12s %-*s", m[k].name, v, uw, m[k].unit);
                }
                f64 base;
                if (baseline_metric(b->cli_id, r, k, &base)) {
                    char bv[32], d[16];
                    format_value(bv, sizeof(bv), base);
                    format_delta(d, sizeof(d), delta_pct(m[k].value, base));
                    tb_printf(&tb, " %12s %8s", bv, d);
                }
                tb_printf(&tb, "\n");
            }
        }
    }
    return tb.len;
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

    toon_table(f, 1, "gpus", s->gpu_count, "name,dedicated_vram,vendor_id,device_id");
    for (u32 i = 0; i < s->gpu_count; ++i) {
        ToonRow r = toon_row(f, 2);
        toon_cell_str(&r, s->gpus[i].name);
        toon_cell_num(&r, (f64)s->gpus[i].dedicated_vram);
        toon_cell_num(&r, s->gpus[i].vendor_id);
        toon_cell_num(&r, s->gpus[i].device_id);
        toon_row_end(&r);
    }

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
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}
