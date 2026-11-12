// Console front end: runs selected benchmarks headless, prints live progress, writes JSON.
#include "core/baseline.h"
#include "core/bench.h"
#include "core/elevation.h"
#include "core/monitor.h"
#include "core/presets.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sensors.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void print_usage() {
    printf(
        "Usage: ez-pc-bench-cli [options]\n"
        "  --list                 List all test ids\n"
        "  --run <patterns>       Comma-separated test ids or patterns (cpu, mem.*, *.latency). Default: all\n"
        "  --out <file.toon.txt>  Write the results file (TOON text format)\n"
        "  --compare <file>       Show the change against a saved results file\n"
        "  --duration <ms>        Measured duration per phase (default 3000)\n"
        "  --threads <n>          Thread count for MT tests (default: all logical)\n"
        "  --gpu <index>          GPU adapter index (default 0 = most powerful)\n"
        "  --disk-drives <list>   Disk test target drive(s), e.g. C or C,D (default C)\n"
        "  --disk-size <MiB>      Disk test file size (default 1024)\n"
        "  --passes <n>           Disk passes (default 3)\n"
        "  --repeat <n>           Run each test n times (max 10) and report the median and spread\n"
        "  --preset <name>        quick, standard or extended: selection + duration / repeat / disk settings;\n"
        "                         --run and later options override it\n"
        "  --write-through        Disk writes with FILE_FLAG_WRITE_THROUGH\n"
        "  --log-sensors <file>   Log sensors (clocks, load, temperatures, battery, power rails) to a .csv or\n"
        "                         .toon.txt file; alone for --seconds, with --run / --preset during the run\n"
        "  --interval <ms>        Sensor log interval (default 1000, minimum 100)\n"
        "  --seconds <n>          Sensor log length without a run (default 60; Ctrl+C stops early)\n"
        "  --sensors <patterns>   Channels to log, e.g. cpu.*,gpu0.*,bat.* (default: all but per-core;\n"
        "                         'all' = everything)\n"
        "  --list-sensors         List the sensor channel ids with their current values\n"
        "  --elevate              Relaunch as administrator in a new console\n"
        "  --pause                Wait for Enter before exiting\n");
}

// '*' wildcard match.
static bool wild_match(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') return wild_match(pat + 1, s) || (*s && wild_match(pat, s + 1));
    return *s == *pat && wild_match(pat + 1, s + 1);
}

// "cpu" matches "cpu.*"; patterns with '*' match literally.
static bool pattern_match(const char* pat, const char* id) {
    if (wild_match(pat, id)) return true;
    if (strchr(pat, '*')) return false;
    size_t n = strlen(pat);
    return strncmp(pat, id, n) == 0 && id[n] == '.';
}

static bool select_patterns(const char* list, bool* selected) {
    char buf[1024];
    strncpy_s(buf, list, _TRUNCATE);
    bool any = false;
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
        bool matched = false;
        for (u32 i = 0; i < g_bench_count; ++i)
            if (pattern_match(tok, g_benches[i].cli_id)) selected[i] = matched = true;
        if (!matched) fprintf(stderr, "warning: '%s' matches no test\n", tok);
        any |= matched;
    }
    return any;
}

static std::atomic<bool> s_ctrl_c;

static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        s_ctrl_c.store(true);
        if (runner_is_running()) {
            runner_cancel();
            return TRUE;
        }
    }
    return FALSE;
}

static void print_result(u32 id) {
    const BenchResult* r = &g_results[id];
    const BenchStatus st = (BenchStatus)g_status[id].load(std::memory_order_acquire);
    printf("\r%-79s\r", "");
    printf("%-9s %s  (%.1f s)%s%s\n", status_name(st), g_benches[id].cli_id, r->seconds, r->error[0] ? "  " : "", r->error);
    if (st != ST_DONE) return;
    if (env_valid(&g_env[id])) {
        char env[256], warn[160];
        env_describe(&g_env[id], env_is_gpu_test(id), env, sizeof(env));
        env_warnings(&g_env[id], warn, sizeof(warn));
        printf("          [%s]%s%s\n", env, warn[0] ? "  WARNING: " : "", warn);
    }
    const Metric* m = res_metrics(r);
    const char* id_str = g_benches[id].cli_id;
    char v[32], x[32];
    if (r->kind == RESULT_MATRIX) {
        printf("          %s [%s] %ux%u\n", m[0].name, m[0].unit, r->rows, r->cols);
        for (u32 row = 0; row < r->rows; ++row) {
            printf("          ");
            for (u32 c = 0; c < r->cols && row * r->cols + c < r->count; ++c) {
                format_value(v, sizeof(v), m[row * r->cols + c].value);
                printf(" %9s", v);
            }
            printf("\n");
        }
        f64 lo, avg, hi, blo, bavg, bhi;
        if (baseline_matrix(id_str, r, &blo, &bavg, &bhi)) {
            res_matrix_stats(r, &lo, &avg, &hi);
            char bv[32], d[16];
            format_value(v, sizeof(v), avg);
            format_value(bv, sizeof(bv), bavg);
            format_delta(d, sizeof(d), delta_pct(avg, bavg));
            printf("          average %s, baseline %s, %s\n", v, bv, d);
        }
        return;
    }
    const int uw = g_baseline.loaded ? 8 : 0;   // pad the unit only when baseline columns follow
    const MetricSpread* sp = res_spread(r);
    for (u32 k = 0; k < r->count; ++k) {
        format_value(v, sizeof(v), m[k].value);
        if (r->kind == RESULT_SERIES) {
            format_x(x, sizeof(x), m[k].x, r->x_unit);
            printf("          %-10s %-16s %12s %-*s", x, m[k].name, v, uw, m[k].unit);
        } else {
            printf("          %-27s %12s %-*s", m[k].name, v, uw, m[k].unit);
        }
        f64 base;
        if (baseline_metric(id_str, r, k, &base)) {
            char bv[32], d[16];
            format_value(bv, sizeof(bv), base);
            format_delta(d, sizeof(d), delta_pct(m[k].value, base));
            printf(" base %10s %8s", bv, d);
        }
        if (sp[k].runs >= 2)
            printf("  median of %u, spread %.1f%%%s", sp[k].runs, sp[k].cv, sp[k].cv >= NOISY_CV_PCT ? " NOISY" : "");
        printf("\n");
    }
}

static void print_live() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    if (cur < 0 || g_run.done_count.load() >= g_run.total) return;
    char v[32];
    format_value(v, sizeof(v), g_live.current.load(std::memory_order_relaxed));
    char line[160];
    char rep[24] = "";
    if (g_run.reps > 1) snprintf(rep, sizeof(rep), " run %u/%u", g_run.rep.load() + 1, g_run.reps);
    snprintf(line, sizeof(line), "[%u/%u] %s%s %3.0f%%  %s %s  %s", g_run.done_count.load() + 1, g_run.total,
             g_benches[cur].cli_id, rep, g_live.progress.load() * 100.0f, v, g_live.unit.load(), g_live.phase.load());
    printf("\r%-79.79s", line);
    fflush(stdout);
}

static void print_sysinfo(const BenchParams* p, const bool* selected) {
    const SysInfo* s = &g_sys;
    printf("CPU   %s, %uC/%uT, L2 %u KiB/core, L3 %u KiB\n", s->cpu.brand, s->cpu.cores, s->cpu.threads,
           s->cpu.l2_kib, s->cpu.l3_kib);
    printf("RAM   %.1f GiB usable", s->ram.total_bytes / (f64)GiB);
    for (u32 i = 0; i < s->ram.module_count; ++i)
        printf("%s %llu GiB %s-%u", i ? "," : " |", s->ram.modules[i].size_bytes / GiB, s->ram.modules[i].type,
               s->ram.modules[i].configured_mts);
    printf("\n");
    for (u32 i = 0; i < s->gpu_count; ++i)
        printf("GPU%u  %s, driver %s%s\n", i, s->gpus[i].name, s->gpus[i].driver[0] ? s->gpus[i].driver : "?",
               s->gpus[i].hags < 0 ? "" : s->gpus[i].hags ? ", HW scheduling on" : ", HW scheduling off");
    if (s->board.sys_product[0] || s->board.board_product[0])
        printf("Board %s %s, %s %s, BIOS %s (%s)\n", s->board.sys_vendor, s->board.sys_product, s->board.board_vendor,
               s->board.board_product, s->board.bios_version, s->board.bios_date);
    for (u32 i = 0; i < s->drive_count; ++i) {
        char d[192];
        drive_describe(&s->drives[i], d, sizeof(d));
        printf("Disk  %c:  %s\n", s->drives[i].letter, d);
    }
    char win[192];
    winstate_describe(&s->win, win, sizeof(win));
    printf("OS    %s, %s\n", s->os, win);
    printf("Power %s, plan \"%s\", mode \"%s\"\n\n", s->power.on_ac ? "AC" : "battery", s->power.scheme_name,
           power_mode_name(s->power.mode));
    if (!s->elevated) printf("WARNING: not running as administrator (use --elevate). Some tests are degraded.\n");
    if (!s->power.on_ac) printf("WARNING: running on battery - results will be lower.\n");
    if (!power_is_best_performance(&s->power)) printf("WARNING: power mode is not \"Best performance\".\n");
    char wl[2][192];
    for (u32 i = 0, n = winstate_warnings(&s->win, wl, 2); i < n; ++i) printf("WARNING: %s\n", wl[i]);
    bool disk_selected = false;
    for (u32 i = 0; i < g_bench_count; ++i) disk_selected |= g_benches[i].suite == SUITE_DISK && selected[i];
    for (u32 d = 0; d < 26 && disk_selected; ++d) {
        if (!((p->disk_drives >> d) & 1)) continue;
        char lines[2][192];
        const u32 n = drive_target_warnings((wchar_t)(L'A' + d), lines);
        for (u32 i = 0; i < n; ++i) printf("WARNING: %s\n", lines[i]);
    }
}

static void to_utf8(const wchar_t* w, char* out, int size) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, nullptr, nullptr);
}

// ---- sensors ------------------------------------------------------------------------------------

static SensorChannel s_channels[CH_MAX];

// Starts the sampler and waits (up to 5 s) for the second sample: rate values (clock, load, power) need two.
static void sensors_warm_up() {
    sensors_start();
    u32 seq = 0;
    for (u32 i = 0; i < 50 && seq < 2; ++i) {
        Sleep(100);
        sensors_channels(s_channels, CH_MAX, &seq);
    }
}

static int list_sensors() {
    sensors_warm_up();
    u32 seq;
    const u32 n = sensors_channels(s_channels, CH_MAX, &seq);
    for (u32 i = 0; i < n; ++i) {
        const SensorChannel* c = &s_channels[i];
        char v[32] = "-";
        if (c->valid) snprintf(v, sizeof(v), "%.*f %s", c->decimals, c->value, c->unit);
        printf("%-22s %-48.48s %14s%s\n", c->id, c->name, v, c->flags & CHF_DETAIL ? "  (detail)" : "");
    }
    sensors_stop();
    return 0;
}

static bool start_sensor_log(const wchar_t* path, u32 interval_ms, const char* patterns) {
    const size_t len = wcslen(path);
    const bool csv = len >= 4 && !_wcsicmp(path + len - 4, L".csv");
    char err[160];
    if (!sensors_log_start(path, csv ? SLOG_CSV : SLOG_TOON, interval_ms, patterns, err, sizeof(err))) {
        fwprintf(stderr, L"Cannot log to %s: ", path);
        fprintf(stderr, "%s\n", err);
        return false;
    }
    return true;
}

static int finish_sensor_log() {
    SensorLogStatus st;
    sensors_log_status(&st);
    sensors_log_stop();
    SensorLogStatus done;
    sensors_log_status(&done);
    if (done.error[0]) { fprintf(stderr, "Sensor log: %s\n", done.error); return 1; }
    printf("Wrote %s (%u rows x %u channels, %.0f s)\n", st.path, st.rows, st.columns, st.seconds);
    printf("Slowest sample %.0f ms: %s\n", st.slowest_ms, st.slowest);
    return 0;
}

// --log-sensors without a run: log for `seconds` (Ctrl+C stops early).
static int log_sensors_only(const wchar_t* path, u32 interval_ms, u32 seconds, const char* patterns) {
    sensors_warm_up();      // first row with every value present
    if (!start_sensor_log(path, interval_ms, patterns)) { sensors_stop(); return 3; }
    sensors_stop();         // the log holds its own reference
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    const f64 t0 = timer_now();
    while (!s_ctrl_c.load() && timer_now() - t0 < seconds) {
        SensorLogStatus st;
        sensors_log_status(&st);
        printf("\rLogging %u channels every %u ms: %u rows, %.0f / %u s ", st.columns, st.interval_ms, st.rows, timer_now() - t0,
               seconds);
        fflush(stdout);
        Sleep(200);
    }
    printf("\r%-79s\r", "");
    return finish_sensor_log();
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    sysinfo_init();
    runner_init();

    bool selected[MAX_BENCHES] = {};
    bool any_run = false, from_preset = false, list = false, pause = false, elevate = false;
    const wchar_t* out_path = nullptr;
    const wchar_t* compare_path = nullptr;
    const wchar_t* sensor_log = nullptr;
    char sensor_patterns[512] = {};
    u32 log_interval = 1000, log_seconds = 60;
    bool list_channels = false;
    BenchParams params;
    params.gpu_adapter = sysinfo_default_gpu();

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        const bool has_val = i + 1 < argc;
        char val[1024] = {};
        if (has_val) to_utf8(argv[i + 1], val, sizeof(val));

        if (!wcscmp(a, L"--list")) list = true;
        else if (!wcscmp(a, L"--pause")) pause = true;
        else if (!wcscmp(a, L"--elevate")) elevate = true;
        else if (!wcscmp(a, L"--write-through")) params.disk_write_through = true;
        else if (!wcscmp(a, L"--run") && has_val) { if (from_preset) memset(selected, 0, sizeof(selected)); from_preset = false; any_run = true; if (!select_patterns(val, selected)) return 3; ++i; }
        else if (!wcscmp(a, L"--out") && has_val) out_path = argv[++i];
        else if (!wcscmp(a, L"--compare") && has_val) compare_path = argv[++i];
        else if (!wcscmp(a, L"--log-sensors") && has_val) sensor_log = argv[++i];
        else if (!wcscmp(a, L"--interval") && has_val) { log_interval = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--seconds") && has_val) { log_seconds = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--sensors") && has_val) { strncpy_s(sensor_patterns, val, _TRUNCATE); ++i; }
        else if (!wcscmp(a, L"--list-sensors")) list_channels = true;
        else if (!wcscmp(a, L"--duration") && has_val) { params.duration_ms = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--threads") && has_val) { params.thread_count = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--gpu") && has_val) { params.gpu_adapter = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--disk-drives") && has_val) {
            params.disk_drives = 0;
            for (const char* d = val; *d; ++d)
                if (towupper(*d) >= L'A' && towupper(*d) <= L'Z') params.disk_drives |= 1u << (towupper(*d) - L'A');
            ++i;
        }
        else if (!wcscmp(a, L"--disk-size") && has_val) { params.disk_file_bytes = (u64)atoi(val) * MiB; ++i; }
        else if (!wcscmp(a, L"--passes") && has_val) { params.disk_passes = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--repeat") && has_val) { params.repeat = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--preset") && has_val) {
            const RunPreset* pr = preset_find(val);
            if (!pr) { fprintf(stderr, "unknown preset '%s' (quick, standard, extended)\n", val); return 3; }
            bool preset_sel[MAX_BENCHES] = {};
            preset_apply(pr, &params, preset_sel);
            if (!any_run) { memcpy(selected, preset_sel, sizeof(selected)); any_run = from_preset = true; }
            ++i;
        }
        else { print_usage(); return a[0] == L'-' && a[1] == L'h' ? 0 : 3; }
    }

    if (list) {
        for (u32 i = 0; i < g_bench_count; ++i)
            printf("%-28s %s / %s / %s\n", g_benches[i].cli_id, g_suites[g_benches[i].suite].name, g_benches[i].group,
                   g_benches[i].name);
        return 0;
    }

    if (list_channels) return list_sensors();
    if (sensor_log && !any_run) return log_sensors_only(sensor_log, log_interval, log_seconds, sensor_patterns);

    if (elevate && !g_sys.elevated) {
        if (relaunch_elevated(L"--pause")) return 0;
        fprintf(stderr, "Elevation declined or failed; continuing without admin rights.\n");
    }

    if (!any_run)
        for (u32 i = 0; i < g_bench_count; ++i) selected[i] = !(g_benches[i].flags & BENCH_DEFAULT_OFF);

    if (compare_path) {
        char err[160];
        if (!baseline_load(compare_path, err, sizeof(err))) {
            fwprintf(stderr, L"Cannot load baseline %s: ", compare_path);
            fprintf(stderr, "%s\n", err);
            return 3;
        }
    }

    print_sysinfo(&params, selected);
    if (g_baseline.loaded)
        printf("Baseline %s (%s), %u metrics; change in %% (+ = larger value)\n\n", g_baseline.file, g_baseline.timestamp,
               g_baseline.count);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (sensor_log && !start_sensor_log(sensor_log, log_interval, sensor_patterns)) return 3;
    if (!runner_start(selected, &params)) { fprintf(stderr, "Nothing to run.\n"); return 3; }

    // Print each finished test once, in run order; live line in between.
    u32 printed = 0;
    for (;;) {
        const bool running = runner_is_running();
        while (printed < g_run.total) {
            u32 id = g_run.list[printed];
            BenchStatus st = (BenchStatus)g_status[id].load(std::memory_order_acquire);
            if (st == ST_PENDING || st == ST_RUNNING) break;
            print_result(id);
            ++printed;
        }
        if (!running) break;
        print_live();
        Sleep(100);
    }
    runner_wait();
    if (sensor_log && finish_sensor_log() != 0) return 1;

    int exit_code = 0;
    for (u32 i = 0; i < g_run.total; ++i) {
        BenchStatus st = (BenchStatus)g_status[g_run.list[i]].load();
        if (st == ST_FAILED) exit_code = 1;
        else if (st == ST_CANCELLED && exit_code == 0) exit_code = 2;
    }
    printf("\r%-79s\rFinished in %.1f s\n", "", g_run.end_time - g_run.start_time);

    if (out_path) {
        if (results_write_toon(out_path, &g_run.params)) wprintf(L"Wrote %s\n", out_path);
        else { fwprintf(stderr, L"Failed to write %s\n", out_path); exit_code = 1; }
    }
    if (pause) {
        printf("Press Enter to exit...");
        getchar();
    }
    return exit_code;
}
