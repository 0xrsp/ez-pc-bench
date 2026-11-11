// Console front end: runs selected benchmarks headless, prints live progress, writes JSON.
#include "core/baseline.h"
#include "core/bench.h"
#include "core/elevation.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
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
        "  --passes <n>           Disk passes (default 5)\n"
        "  --write-through        Disk writes with FILE_FLAG_WRITE_THROUGH\n"
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

static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
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
        printf("\n");
    }
}

static void print_live() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    if (cur < 0 || g_run.done_count.load() >= g_run.total) return;
    char v[32];
    format_value(v, sizeof(v), g_live.current.load(std::memory_order_relaxed));
    char line[160];
    snprintf(line, sizeof(line), "[%u/%u] %s %3.0f%%  %s %s  %s", g_run.done_count.load() + 1, g_run.total,
             g_benches[cur].cli_id, g_live.progress.load() * 100.0f, v, g_live.unit.load(), g_live.phase.load());
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
    for (u32 i = 0; i < s->gpu_count; ++i) printf("GPU%u  %s\n", i, s->gpus[i].name);
    for (u32 i = 0; i < s->drive_count; ++i) {
        char d[192];
        drive_describe(&s->drives[i], d, sizeof(d));
        printf("Disk  %c:  %s\n", s->drives[i].letter, d);
    }
    printf("OS    %s\n", s->os);
    printf("Power %s, plan \"%s\", mode \"%s\"\n\n", s->power.on_ac ? "AC" : "battery", s->power.scheme_name,
           power_mode_name(s->power.mode));
    if (!s->elevated) printf("WARNING: not running as administrator (use --elevate). Some tests are degraded.\n");
    if (!s->power.on_ac) printf("WARNING: running on battery - results will be lower.\n");
    if (!power_is_best_performance(&s->power)) printf("WARNING: power mode is not \"Best performance\".\n");
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

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    sysinfo_init();
    runner_init();

    bool selected[MAX_BENCHES] = {};
    bool any_run = false, list = false, pause = false, elevate = false;
    const wchar_t* out_path = nullptr;
    const wchar_t* compare_path = nullptr;
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
        else if (!wcscmp(a, L"--run") && has_val) { any_run = true; if (!select_patterns(val, selected)) return 3; ++i; }
        else if (!wcscmp(a, L"--out") && has_val) out_path = argv[++i];
        else if (!wcscmp(a, L"--compare") && has_val) compare_path = argv[++i];
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
        else { print_usage(); return a[0] == L'-' && a[1] == L'h' ? 0 : 3; }
    }

    if (list) {
        for (u32 i = 0; i < g_bench_count; ++i)
            printf("%-28s %s / %s / %s\n", g_benches[i].cli_id, g_suites[g_benches[i].suite].name, g_benches[i].group,
                   g_benches[i].name);
        return 0;
    }

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
