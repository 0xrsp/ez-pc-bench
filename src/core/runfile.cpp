// Flat results reader (see runfile.h).
#include "core/runfile.h"
#include "core/baseline.h"
#include "core/monitor.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/toon.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static RunFile s_scratch;
RunFile* rf_scratch() { return &s_scratch; }

static void copy_str(char* dst, u32 size, const char* src) { snprintf(dst, size, "%s", src ? src : ""); }

bool rf_wild(const char* pat, const char* s) {
    for (;;) {
        if (*pat == '*') {
            while (*pat == '*') ++pat;
            if (!*pat) return true;
            for (; *s; ++s)
                if (rf_wild(pat, s)) return true;
            return false;
        }
        if (*pat != *s) return false;
        if (!*s) return true;
        ++pat;
        ++s;
    }
}

i32 rf_find_test(const RunFile* rf, const char* id) {
    for (u32 i = 0; i < rf->test_count; ++i)
        if (!strcmp(rf->tests[i].id, id)) return (i32)i;
    return -1;
}

i32 rf_find_metric(const RunFile* rf, u32 test, const char* name, f64 x) {
    const bool wild = strchr(name, '*') != nullptr;
    for (u32 i = 0; i < rf->metric_count; ++i) {
        const RfMetric* m = &rf->metrics[i];
        if (m->test != test || m->x != x) continue;
        if (wild ? rf_wild(name, m->name) : !strcmp(name, m->name)) return (i32)i;
    }
    return -1;
}

static i32 add_test(RunFile* rf, const char* id) {
    const i32 t = rf_find_test(rf, id);
    if (t >= 0 || rf->test_count >= RF_MAX_TESTS) return t;
    RfTest* nt = &rf->tests[rf->test_count];
    memset(nt, 0, sizeof(*nt));
    copy_str(nt->id, sizeof(nt->id), id);
    nt->cpu_util = NAN;
    return (i32)rf->test_count++;
}

static void add_metric(RunFile* rf, i32 test, const char* name, const char* unit, f64 value, f64 x) {
    if (test < 0 || rf->metric_count >= RF_MAX_METRICS || !isfinite(value)) return;
    RfMetric* m = &rf->metrics[rf->metric_count++];
    m->test = (u16)test;
    copy_str(m->name, sizeof(m->name), name);
    copy_str(m->unit, sizeof(m->unit), unit);
    m->value = value;
    m->x = x;
    m->cv = NAN;
}

static void reset(RunFile* rf) {
    // Only the header is cleared; the arrays are filled up to their counts.
    memset(rf, 0, offsetof(RunFile, tests));
    rf->metric_count = 0;
    rf->memory_integrity = -1;
}

// ---- TOON ---------------------------------------------------------------------------------------------

static i32 field(const ToonLine* l, const char* name) {
    for (u32 i = 0; i < l->field_count; ++i)
        if (!strcmp(l->fields[i], name)) return (i32)i;
    return -1;
}

static const char* cell(char cells[][TOON_CELL], i32 i) { return i >= 0 ? cells[i] : ""; }

static f64 num(const char* s) {
    char* end = nullptr;
    const f64 v = strtod(s, &end);
    return end == s ? NAN : v;
}

static void read_table(ToonReader* r, const ToonLine* l, const char* path, RunFile* rf) {
    static char cells[TOON_MAX_FIELDS][TOON_CELL];
    const bool tests = !strcmp(path, "tests"), metrics = !strcmp(path, "metrics"), spread = !strcmp(path, "spread");
    const bool env = !strcmp(path, "env"), modules = !strcmp(path, "system.ram.modules");
    const i32 f_id = field(l, "id"), f_name = field(l, "name"), f_unit = field(l, "unit"), f_value = field(l, "value");
    const i32 f_x = field(l, "x"), f_cv = field(l, "cv"), f_util = field(l, "cpu_util"), f_flags = field(l, "flags");
    const char *s, *e;
    u32 indent;
    for (u32 row = 0; row < l->count; ++row) {
        const char* before = r->p;
        const u32 line_before = r->line;
        if (!toon_next_line(r, &s, &e, &indent)) return;
        if (indent == 0) { r->p = before; r->line = line_before; return; }      // table shorter than its count
        if (!tests && !metrics && !spread && !env && !modules) continue;
        const u32 n = toon_split(s, e, cells, TOON_MAX_FIELDS);
        if (n != l->field_count) continue;
        if (tests) {
            add_test(rf, cell(cells, f_id));
        } else if (metrics) {
            add_metric(rf, add_test(rf, cell(cells, f_id)), cell(cells, f_name), cell(cells, f_unit),
                       num(cell(cells, f_value)), f_x >= 0 ? num(cell(cells, f_x)) : 0.0);
        } else if (spread) {
            const i32 t = rf_find_test(rf, cell(cells, f_id));
            if (t < 0) continue;
            const f64 x = f_x >= 0 ? num(cell(cells, f_x)) : 0.0;
            for (u32 i = 0; i < rf->metric_count; ++i) {
                RfMetric* m = &rf->metrics[i];
                if (m->test == (u16)t && m->x == x && !strcmp(m->name, cell(cells, f_name))) m->cv = (f32)num(cell(cells, f_cv));
            }
        } else if (env) {
            const i32 t = add_test(rf, cell(cells, f_id));
            if (t < 0) continue;
            rf->has_env = true;
            rf->tests[t].has_env = true;
            rf->tests[t].cpu_util = (f32)num(cell(cells, f_util));
            rf->tests[t].env_flags = env_parse_flags(cell(cells, f_flags));
        } else if (modules && row == 0) {
            copy_str(rf->ram_type, sizeof(rf->ram_type), cell(cells, field(l, "type")));
            rf->ram_mts = (u32)atoi(cell(cells, field(l, "configured_mts")));
        }
    }
}

bool rf_parse(const char* data, u32 size, RunFile* rf, char* err, u32 err_size) {
    reset(rf);
    ToonReader r = { data, data + size, 0 };
    if (size >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) r.p += 3;
    char keys[8][64] = {};
    bool app = false;
    f64 installed = 0.0, total = 0.0;
    const char *s, *e;
    u32 indent;
    while (toon_next_line(&r, &s, &e, &indent)) {
        const u32 depth = indent / 2;
        ToonLine l;
        if (depth >= 8 || !toon_parse_key(s, e, &l)) continue;
        copy_str(keys[depth], sizeof(keys[0]), l.key);
        char path[256] = {};
        for (u32 d = 0; d <= depth; ++d) {
            if (d) strcat_s(path, ".");
            strcat_s(path, keys[d]);
        }
        if (l.array && l.field_count) { read_table(&r, &l, path, rf); continue; }
        char v[TOON_CELL];
        toon_decode(l.value, l.value_end, v, sizeof(v));
        if (!strcmp(path, "app")) app = !strcmp(v, "ez-pc-bench");
        else if (!strcmp(path, "kind")) copy_str(rf->kind, sizeof(rf->kind), v);
        else if (!strcmp(path, "timestamp")) copy_str(rf->timestamp, sizeof(rf->timestamp), v);
        else if (!strcmp(path, "system.cpu.brand")) copy_str(rf->cpu, sizeof(rf->cpu), v);
        else if (!strcmp(path, "system.cpu.threads")) rf->cpu_threads = (u32)atoi(v);
        else if (!strcmp(path, "system.ram.installed_bytes")) installed = num(v);
        else if (!strcmp(path, "system.ram.total_bytes")) total = num(v);
        else if (!strcmp(path, "system.os")) copy_str(rf->os, sizeof(rf->os), v);
        else if (!strcmp(path, "system.elevated")) rf->elevated = !strcmp(v, "true");
        else if (!strcmp(path, "system.power.on_ac")) rf->on_ac = !strcmp(v, "true");
        else if (!strcmp(path, "system.power.mode")) copy_str(rf->power_mode, sizeof(rf->power_mode), v);
        else if (!strcmp(path, "system.windows.memory_integrity"))
            rf->memory_integrity = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        else if (!strncmp(path, "params.", 7)) {
            rf->has_params = true;
            const char* k = path + 7;
            if (!strcmp(k, "gpu")) copy_str(rf->gpu, sizeof(rf->gpu), v);
            else if (!strcmp(k, "duration_ms")) rf->duration_ms = num(v);
            else if (!strcmp(k, "thread_count")) rf->thread_count = num(v);
            else if (!strcmp(k, "gpu_width")) rf->gpu_width = num(v);
            else if (!strcmp(k, "gpu_height")) rf->gpu_height = num(v);
            else if (!strcmp(k, "disk_file_bytes")) rf->disk_file_bytes = num(v);
            else if (!strcmp(k, "disk_passes")) rf->disk_passes = num(v);
            else if (!strcmp(k, "repeat")) rf->repeat = num(v);
            else if (!strcmp(k, "disk_write_through")) rf->disk_write_through = !strcmp(v, "true");
            else if (!strcmp(k, "disk_drives"))
                for (const char* p = v; *p; ++p)
                    if (*p >= 'A' && *p <= 'Z') rf->disk_drives |= 1u << (*p - 'A');
        }
    }
    rf->ram_bytes = installed > 0.0 ? installed : total;
    if (!app) { snprintf(err, err_size, "not an EzPcBench results file"); return false; }
    return true;
}

// ---- the run in memory --------------------------------------------------------------------------------

void rf_from_current(RunFile* rf) {
    reset(rf);
    const SysInfo* s = &g_sys;
    const BenchParams* p = &g_run.params;
    time_t now = time(nullptr);
    struct tm t_local;
    localtime_s(&t_local, &now);
    strftime(rf->timestamp, sizeof(rf->timestamp), "%Y-%m-%dT%H:%M:%S", &t_local);
    copy_str(rf->cpu, sizeof(rf->cpu), s->cpu.brand);
    copy_str(rf->gpu, sizeof(rf->gpu), p->gpu_adapter < s->gpu_count ? s->gpus[p->gpu_adapter].name : "");
    copy_str(rf->os, sizeof(rf->os), s->os);
    rf->cpu_threads = s->cpu.threads;
    rf->ram_bytes = (f64)(s->ram.installed_bytes ? s->ram.installed_bytes : s->ram.total_bytes);
    if (s->ram.module_count) {
        copy_str(rf->ram_type, sizeof(rf->ram_type), s->ram.modules[0].type);
        rf->ram_mts = s->ram.modules[0].configured_mts;
    }
    rf->on_ac = s->power.on_ac;
    rf->elevated = s->elevated;
    copy_str(rf->power_mode, sizeof(rf->power_mode), power_mode_name(s->power.mode));
    rf->memory_integrity = s->win.hvci;
    rf->has_params = true;
    rf->duration_ms = p->duration_ms;
    rf->thread_count = p->thread_count;
    rf->gpu_width = p->gpu_width;
    rf->gpu_height = p->gpu_height;
    rf->disk_file_bytes = (f64)p->disk_file_bytes;
    rf->disk_passes = p->disk_passes;
    rf->repeat = p->repeat;
    rf->disk_write_through = p->disk_write_through;
    rf->disk_drives = p->disk_drives;

    for (u32 i = 0; i < g_bench_count; ++i) {
        if (g_status[i].load(std::memory_order_acquire) != ST_DONE) continue;
        const i32 t = add_test(rf, g_benches[i].cli_id);
        if (t < 0) continue;
        if (env_valid(&g_env[i])) {
            rf->has_env = true;
            rf->tests[t].has_env = true;
            rf->tests[t].cpu_util = g_env[i].cpu_util;
            rf->tests[t].env_flags = g_env[i].flags;
        }
        const BenchResult* r = &g_results[i];
        const Metric* m = res_metrics(r);
        const MetricSpread* sp = res_spread(r);
        for (u32 k = 0; k < r->count; ++k) {
            add_metric(rf, t, m[k].name, m[k].unit, m[k].value, baseline_key_x(r, k));
            if (sp[k].runs >= 2 && rf->metric_count) rf->metrics[rf->metric_count - 1].cv = sp[k].cv;
        }
    }
}
