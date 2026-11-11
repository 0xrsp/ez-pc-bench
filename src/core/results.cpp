#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
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
        tb_printf(&tb, "Disk %c: %s%s\n", s->drives[i].letter, d, s->drives[i].letter == (char)p->disk_drive ? "  [target]" : "");
    }
    tb_printf(&tb, "OS:    %s, %s\n", s->os, s->elevated ? "elevated" : "not elevated");
    tb_printf(&tb, "Power: %s, plan \"%s\", mode \"%s\"\n\n", s->power.on_ac ? "AC" : "battery", s->power.scheme_name,
              power_mode_name(s->power.mode));

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
        } else {
            for (u32 k = 0; k < r->count; ++k) {
                format_value(v, sizeof(v), m[k].value);
                if (r->kind == RESULT_SERIES) {
                    format_x(x, sizeof(x), m[k].x, r->x_unit);
                    tb_printf(&tb, "    %-12s %-20s %12s %s\n", x, m[k].name, v, m[k].unit);
                } else {
                    tb_printf(&tb, "    %-33s %12s %s\n", m[k].name, v, m[k].unit);
                }
            }
        }
    }
    return tb.len;
}

// ---- JSON --------------------------------------------------------------------

static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void json_num(FILE* f, f64 v) {
    if (isfinite(v)) fprintf(f, "%.17g", v);
    else fputs("null", f);
}

static const char* kind_name(ResultKind k) {
    return k == RESULT_SERIES ? "series" : k == RESULT_MATRIX ? "matrix" : "values";
}

bool results_write_json(const wchar_t* path, const BenchParams* p) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    const SysInfo* s = &g_sys;

    char stamp[32];
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_s(&tm_local, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);

    fprintf(f, "{\n  \"app\": \"ez-pc-bench\",\n  \"timestamp\": ");
    json_str(f, stamp);
    fprintf(f, ",\n  \"system\": {\n    \"cpu\": { \"brand\": ");
    json_str(f, s->cpu.brand);
    fprintf(f, ", \"vendor\": ");
    json_str(f, s->cpu.vendor);
    fprintf(f, ", \"family\": %u, \"model\": %u, \"stepping\": %u, \"cores\": %u, \"threads\": %u, "
               "\"l1d_kib\": %u, \"l2_kib\": %u, \"l3_kib\": %u, \"avx2\": %s, \"avx512f\": %s },\n",
            s->cpu.family, s->cpu.model, s->cpu.stepping, s->cpu.cores, s->cpu.threads, s->cpu.l1d_kib,
            s->cpu.l2_kib, s->cpu.l3_kib, s->cpu.avx2 ? "true" : "false", s->cpu.avx512f ? "true" : "false");
    fprintf(f, "    \"ram\": { \"total_bytes\": %llu, \"installed_bytes\": %llu, \"modules\": [",
            s->ram.total_bytes, s->ram.installed_bytes);
    for (u32 i = 0; i < s->ram.module_count; ++i) {
        const RamModule* m = &s->ram.modules[i];
        fprintf(f, "%s\n      { \"type\": ", i ? "," : "");
        json_str(f, m->type);
        fprintf(f, ", \"size_bytes\": %llu, \"speed_mts\": %u, \"configured_mts\": %u, \"manufacturer\": ",
                m->size_bytes, m->speed_mts, m->configured_mts);
        json_str(f, m->manufacturer);
        fprintf(f, ", \"part\": ");
        json_str(f, m->part);
        fprintf(f, ", \"locator\": ");
        json_str(f, m->locator);
        fprintf(f, " }");
    }
    fprintf(f, " ] },\n    \"gpus\": [");
    for (u32 i = 0; i < s->gpu_count; ++i) {
        fprintf(f, "%s\n      { \"name\": ", i ? "," : "");
        json_str(f, s->gpus[i].name);
        fprintf(f, ", \"dedicated_vram\": %llu, \"vendor_id\": %u, \"device_id\": %u }", s->gpus[i].dedicated_vram,
                s->gpus[i].vendor_id, s->gpus[i].device_id);
    }
    fprintf(f, " ],\n    \"drives\": [");
    for (u32 i = 0; i < s->drive_count; ++i) {
        const DriveInfo* d = &s->drives[i];
        fprintf(f, "%s\n      { \"letter\": \"%c\", \"model\": ", i ? "," : "", d->letter);
        json_str(f, d->model);
        fprintf(f, ", \"bus\": ");
        json_str(f, d->bus);
        fprintf(f, ", \"fs\": ");
        json_str(f, d->fs);
        fprintf(f, ", \"total_bytes\": %llu, \"logical_sector\": %u, \"physical_sector\": %u, \"virtual_disk\": %s, "
                   "\"dev_drive\": %s, \"bitlocker\": %d }",
                d->total_bytes, d->logical_sector, d->physical_sector, d->virtual_disk ? "true" : "false",
                d->dev_drive ? "true" : "false", d->bitlocker);
    }
    fprintf(f, " ],\n    \"os\": ");
    json_str(f, s->os);
    fprintf(f, ",\n    \"elevated\": %s,\n    \"power\": { \"on_ac\": %s, \"plan\": ", s->elevated ? "true" : "false",
            s->power.on_ac ? "true" : "false");
    json_str(f, s->power.scheme_name);
    fprintf(f, ", \"mode\": ");
    json_str(f, power_mode_name(s->power.mode));
    fprintf(f, " }\n  },\n");

    fprintf(f, "  \"params\": { \"duration_ms\": %u, \"thread_count\": %u, \"gpu_adapter\": %u, \"gpu_width\": %u, "
               "\"gpu_height\": %u, \"disk_drive\": \"%c\", \"disk_file_bytes\": %llu, \"disk_passes\": %u, "
               "\"disk_write_through\": %s },\n",
            p->duration_ms, p->thread_count, p->gpu_adapter, p->gpu_width, p->gpu_height, (char)p->disk_drive,
            p->disk_file_bytes, p->disk_passes, p->disk_write_through ? "true" : "false");

    fprintf(f, "  \"results\": [");
    bool first = true;
    for (u32 i = 0; i < g_bench_count; ++i) {
        BenchStatus st = (BenchStatus)g_status[i].load(std::memory_order_acquire);
        if (st == ST_NOT_RUN) continue;
        const BenchDesc* b = &g_benches[i];
        const BenchResult* r = &g_results[i];
        fprintf(f, "%s\n    { \"id\": ", first ? "" : ",");
        first = false;
        json_str(f, b->cli_id);
        fprintf(f, ", \"suite\": ");
        json_str(f, g_suites[b->suite].name);
        fprintf(f, ", \"group\": ");
        json_str(f, b->group);
        fprintf(f, ", \"name\": ");
        json_str(f, b->name);
        fprintf(f, ", \"status\": ");
        json_str(f, status_name(st));
        fprintf(f, ", \"seconds\": ");
        json_num(f, r->seconds);
        fprintf(f, ", \"error\": ");
        json_str(f, r->error);
        fprintf(f, ", \"kind\": ");
        json_str(f, kind_name(r->kind));
        if (r->kind == RESULT_MATRIX) fprintf(f, ", \"rows\": %u, \"cols\": %u", r->rows, r->cols);
        if (r->kind == RESULT_SERIES) {
            fprintf(f, ", \"x_name\": ");
            json_str(f, r->x_name);
            fprintf(f, ", \"x_unit\": ");
            json_str(f, r->x_unit);
        }
        fprintf(f, ", \"metrics\": [");
        const Metric* m = res_metrics(r);
        for (u32 k = 0; k < r->count && st == ST_DONE; ++k) {
            fprintf(f, "%s{ \"name\": ", k ? ", " : "");
            json_str(f, m[k].name);
            fprintf(f, ", \"unit\": ");
            json_str(f, m[k].unit);
            fprintf(f, ", \"value\": ");
            json_num(f, m[k].value);
            if (r->kind == RESULT_SERIES) { fprintf(f, ", \"x\": "); json_num(f, m[k].x); }
            fprintf(f, " }");
        }
        fprintf(f, "] }");
    }
    fprintf(f, "\n  ]\n}\n");
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}
