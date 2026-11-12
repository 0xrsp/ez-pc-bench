// Console front end: runs selected benchmarks headless, prints live progress, writes JSON.
#include "core/adv_sensors.h"
#include "app/stability_cli.h"
#include "core/baseline.h"
#include "core/bench.h"
#include "core/disk_health.h"
#include "core/compare.h"
#include "core/elevation.h"
#include "core/history.h"
#include "core/integrity.h"
#include "core/monitor.h"
#include "core/presets.h"
#include "core/report.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sensors.h"
#include "core/sysinfo.h"
#include "core/sysinfo_spd.h"
#include "core/timer.h"
#include "frames/frame_capture.h"
#include "core/score.h"
#include "core/sysinfo.h"
#include "core/toon.h"
#include "core/sysinfo_ext.h"
#include "disk/disk.h"
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
        "  --gpu <index>          GPU adapter index (default 0 = most powerful); a list (0,1) runs the GPU\n"
        "                         suites on each adapter, metrics prefixed with the adapter's short name\n"
        "  --gpu-present <mode>   3D / 2D tests: offscreen (default), windowed or fullscreen (real swapchain,\n"
        "                         vsync off, tearing; Esc in the benchmark window stops the test)\n"
        "  --disk-drives <list>   Disk test target drive(s), e.g. C or C,D (default C)\n"
        "  --disk-size <MiB>      Disk test file size (default 1024)\n"
        "  --passes <n>           Disk passes (default 3)\n"
        "  --repeat <n>           Run each test n times (max 10) and report the median and spread\n"
        "  --sustain <minutes>    Sustained mode: loop each test this long (decimals allowed, >= 2 passes);\n"
        "                         reports the median, first / last pass, min / max and the drop. Overrides --repeat\n"
        "  --preset <name>        quick, standard, extended, sustained or score: selection + duration / repeat /\n"
        "                         disk settings; --run and later options override it\n"
        "  --score [files...]     Print the overall score breakdown (score v1, preliminary): of the given\n"
        "                         results files without running anything, or of this run after it finishes\n"
        "  --score-write          With --score files: also (re)write each file's score block\n"
        "  --html <file.html>     Write a self-contained HTML report of this run\n"
        "  --no-history           Don't save this run to results\\history\\ next to the exe\n"
        "  --write-through        Disk writes with FILE_FLAG_WRITE_THROUGH\n"
        "  --health [--hide-serial]  Print the disk health report of every drive and exit\n"
        "  --log-sensors <file>   Log sensors (clocks, load, temperatures, battery, power rails) to a CSV file\n"
        "                         (any name); alone for --seconds, with --run / --preset during the run\n"
        "  --interval <ms>        Sensor log interval (default 1000, minimum 100)\n"
        "  --seconds <n>          Length of a sensor log without a run (default 60; Ctrl+C stops early)\n"
        "                         or of a frame capture (default 20)\n"
        "  --sensors <patterns>   Channels to log, e.g. cpu.*,gpu0.*,bat.* (default: all but per-core;\n"
        "                         'all' = everything)\n"
        "  --list-sensors         List the sensor channel ids with their current values\n"
        "  --sysinfo [base]       Print the full system report (queries Vulkan / D3D12 on every GPU, which\n"
        "                         wakes a sleeping laptop GPU); with a path, also write <base>.txt / .toon.txt\n"
        "  --disk-profile <name>  Select a CrystalDiskMark profile's disk tests: default, nvme, peak, realworld, demo\n"
        "  --disk-mode <mode>     Disk rows run rw (read + write, default), read, write or mix (adds a mixed pass)\n"
        "  --disk-mix <read %%>   Reads in the mixed pass (default 70)\n"
        "  --disk-data <kind>     Test data: random (default) or zero (all 0x00, shows controller compression)\n"
        "  --disk-folder <path>   Test in this folder instead of on drive letters (network share, USB stick...)\n"
        "  --disk-buffered        Disk I/O through the OS file cache (default: unbuffered)\n"
        "  --disk-interval <ms>   Pause before each disk test (lets the drive cool down)\n"
        "  --disk-custom <rows>   Rows for disk.custom1-4, ';'-separated, e.g. \"RND4K Q32T4 R70;SEQ1M Q8T1\"\n"
        "  --sustained-gib <n>    Cap of the sustained-write test (default 64)\n"
        "  --yes                  Allow tests that write a lot or run very long (disk.sustained.write, disk.raw.scan)\n"
        "  --elevate              Relaunch as administrator in a new console\n"
        "  --adv-report           Print the PawnIO advanced sensors (status, one sample, SPD, timings, PM table)\n"
        "  --no-adv-sensors       Don't use PawnIO even if it is installed\n"
        "  --capture <exe|pid|*>  Frame capture of another application (ETW present events; needs admin):\n"
        "                         fps, frame times, 1%% / 0.1%% lows. * = the busiest presenting process\n"
        "  --capture-etl <file>   Analyze present events from a recorded .etl trace instead (--capture = pid)\n"
        "  --capture-selftest     Check the event decoding and frame analysis on synthetic events\n"
        "  --pause                Wait for Enter before exiting\n"
        "  --stress <tests> ...   Stability tester mode (options below)\n\n");
    stability_cli_usage();
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
        if (g_capture.state.load() == CAP_RUNNING) {
            capture_stop();
            return TRUE;
        }
        if (runner_is_running()) {
            runner_cancel();
            return TRUE;
        }
    }
    return FALSE;
}

// Sustained mode: first / last / min / max / median per tracked metric and the drop over the passes.
static void print_sustain(u32 id) {
    const SustainRecord* s = &g_sustain[id];
    if (s->passes < 2) return;
    printf("          sustained: %u passes in %.0f s\n", s->passes, s->seconds);
    for (u32 k = 0; k < s->tracked; ++k) {
        const Metric* m = sustain_metric(id, k);
        if (!strcmp(m->unit, "x")) continue;
        SustainStats st;
        sustain_stats(s, k, m->unit, &st);
        char v[5][32];
        const f64 vals[] = { st.first, st.last, st.min, st.max, st.median };
        for (u32 j = 0; j < 5; ++j) format_value(v[j], sizeof(v[j]), vals[j]);
        printf("          %-27s first %s, last %s, min %s, max %s, median %s %s, drop %.1f%%\n",
               s->averaged ? "average" : m->name, v[0], v[1], v[2], v[3], v[4], m->unit, st.drop_pct);
    }
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
    print_sustain(id);
}

static void print_live() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    if (cur < 0 || g_run.done_count.load() >= g_run.total) return;
    char v[32];
    format_value(v, sizeof(v), g_live.current.load(std::memory_order_relaxed));
    char line[160];
    char rep[40] = "";
    if (g_run.reps > 1) snprintf(rep, sizeof(rep), " run %u/%u", g_run.rep.load() + 1, g_run.reps);
    if (g_run.sustain_s)
        snprintf(rep, sizeof(rep), " pass %u, %.0f/%u s", g_run.rep.load() + 1,
                 timer_now() - g_run.test_start.load(), g_run.sustain_s);
    snprintf(line, sizeof(line), "[%u/%u] %s%s %3.0f%%  %s %s  %s", g_run.done_count.load() + 1, g_run.total,
             g_benches[cur].cli_id, rep, g_live.progress.load() * 100.0f, v, g_live.unit.load(), g_live.phase.load());
    printf("\r%-79.79s", line);
    fflush(stdout);
}

static void print_sysinfo(const BenchParams* p, const bool* selected) {
    const SysInfo* s = &g_sys;
    printf("CPU   %s, %uC/%uT, L2 %u KiB/core, L3 %u KiB\n", s->cpu.brand, s->cpu.cores, s->cpu.threads,
           s->cpu.l2_kib, s->cpu.l3_kib);
    if (g_cpux.codename[0] || g_cpux.microcode)
        printf("      %s (%s), microcode 0x%08X, base %u MHz%s\n", g_cpux.codename[0] ? g_cpux.codename : "?",
               g_cpux.microarch, g_cpux.microcode, g_cpux.base_mhz, g_cpux.hybrid ? ", hybrid" : "");
    printf("RAM   %.1f GiB usable", s->ram.total_bytes / (f64)GiB);
    for (u32 i = 0; i < s->ram.module_count; ++i)
        printf("%s %llu GiB %s-%u", i ? "," : " |", s->ram.modules[i].size_bytes / GiB, s->ram.modules[i].type,
               s->ram.modules[i].configured_mts);
    printf("\n");
    char timings[192];      // PawnIO only (core/sysinfo_spd.h)
    if (memdetail_timings_line(timings, sizeof(timings))) printf("      %s\n", timings);
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

// --health: the disk health window's text report for every drive.
static int print_health(bool hide_serial) {
    static DiskHealth drives[PHYSDISK_MAX];
    const u32 n = disk_health_scan(drives, PHYSDISK_MAX);
    const u32 size = 256 * 1024;
    char* text = (char*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!text) return 1;
    const u32 len = disk_health_report(drives, n, hide_serial, text, size);
    u32 k = 0;
    for (u32 i = 0; i < len; ++i)
        if (text[i] != '\r') text[k++] = text[i];      // CRLF report; stdout is in text mode
    text[k] = 0;
    fputs(text, stdout);
    VirtualFree(text, 0, MEM_RELEASE);
    disk_health_close();
    if (!g_sys.elevated) printf("Not running as administrator: SATA / USB SMART data and self-tests need --elevate.\n");
    return 0;
}

// --sysinfo: full system report on stdout, optionally saved as text + TOON.
static int print_report(const wchar_t* save_base) {
    gpu_api_details_query_all();
    static SysReport rep;
    static char text[512 * 1024];
    sysreport_build(&rep);
    sysreport_text(&rep, text, sizeof(text));
    fputs(text, stdout);
    if (!save_base) return 0;
    if (!sysreport_save(save_base)) { fwprintf(stderr, L"Failed to write %s.txt\n", save_base); return 1; }
    wprintf(L"\nWrote %s.txt and %s.toon.txt\n", save_base, save_base);
    return 0;
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

// Always CSV, under the name as given (also when it doesn't end in .csv).
static bool start_sensor_log(const wchar_t* path, u32 interval_ms, const char* patterns) {
    char err[160];
    if (!sensors_log_start(path, interval_ms, patterns, err, sizeof(err))) {
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

// --capture: record (or read from an .etl) present events of another process, print and save the result.
static int run_capture(const char* target, f64 seconds, const wchar_t* etl, const wchar_t* out_path) {
    char err[320];
    if (etl) {
        if (!capture_load_etl(etl, target, err, sizeof(err))) { fprintf(stderr, "Capture failed: %s\n", err); return 1; }
    } else {
        if (!capture_allowed())
            printf("Note: not elevated and not in Performance Log Users; the ETW session will most likely be refused.\n");
        if (!capture_start(target, seconds, err, sizeof(err))) { fprintf(stderr, "Capture failed: %s\n", err); return 1; }
        SetConsoleCtrlHandler(on_ctrl, TRUE);
        u32 last = 0;
        f64 last_t = timer_now();
        while (g_capture.state.load() == CAP_RUNNING) {
            Sleep(250);
            const f64 now = timer_now();
            const u32 p = g_capture.presents.load();
            printf("\rCapturing %s: %4.1f / %.0f s, %u presents (%.0f /s)   ", target, now - g_capture.start, seconds, p,
                   (p - last) / (now - last_t));
            fflush(stdout);
            last = p;
            last_t = now;
        }
        capture_wait();
        printf("\n");
        if (g_capture.state.load() != CAP_DONE) { fprintf(stderr, "Capture failed: %s\n", g_capture.error); return 1; }
    }
    const FrameCapture* c = capture_result();
    printf("Process  %s (pid %u), %s%s, %.1f s, %u frames\n", c->process, c->pid, c->api,
           c->tearing < 0 ? "" : c->tearing ? ", tearing" : ", no tearing", c->seconds, c->frames);
    printf("FPS      avg %.1f, 1%% low %.1f, 0.1%% low %.1f (from p99 / p99.9 frame time), 1%% low average %.1f\n",
           c->avg_fps, 1000.0 / c->p99_ms, 1000.0 / c->p999_ms, c->low1_avg_fps);
    printf("Frame ms avg %.3f, median %.3f, p95 %.3f, p99 %.3f, p99.9 %.3f, max %.3f\n", c->avg_ms, c->p50_ms, c->p95_ms,
           c->p99_ms, c->p999_ms, c->max_ms);
    if (c->present_call_ms == c->present_call_ms) printf("Present() avg %.3f ms, sync interval %d\n", c->present_call_ms, c->sync_interval);
    if (c->dropped) printf("WARNING: %u events lost\n", c->dropped);
    if (out_path) {
        if (!capture_write_toon(out_path, c)) { fwprintf(stderr, L"Failed to write %s\n", out_path); return 1; }
        wprintf(L"Wrote %s\n", out_path);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    sysinfo_init();
    for (int i = 1; i < argc; ++i)      // stability tester: its own mode and options
        if (!wcscmp(argv[i], L"--stress") || !wcscmp(argv[i], L"--whea-history")) return stability_cli(argc, argv);
    runner_init();

    bool selected[MAX_BENCHES] = {};
    bool any_run = false, from_preset = false, list = false, pause = false, elevate = false, health = false,
         hide_serial = false, adv_report = false, yes = false;
    const wchar_t* out_path = nullptr;
    const wchar_t* compare_path = nullptr;
    const wchar_t* sensor_log = nullptr;
    char sensor_patterns[512] = {};
    u32 log_interval = 1000, log_seconds = 60;
    bool list_channels = false;
    const wchar_t* capture_etl = nullptr;
    char capture_target[256] = {};
    f64 capture_seconds = 20.0;
    bool capture = false, capture_check = false;
    const wchar_t* html_path = nullptr;
    const wchar_t* score_files[64];
    u32 score_file_count = 0;
    bool score = false, score_write = false, no_history = false;
    BenchParams params;
    params.gpu_adapter = sysinfo_default_gpu();

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        const bool has_val = i + 1 < argc;
        char val[1024] = {};
        if (has_val) to_utf8(argv[i + 1], val, sizeof(val));

        if (!wcscmp(a, L"--health")) health = true;
        else if (!wcscmp(a, L"--hide-serial")) hide_serial = true;
        else if (!wcscmp(a, L"--list")) list = true;
        else if (!wcscmp(a, L"--pause")) pause = true;
        else if (!wcscmp(a, L"--elevate")) elevate = true;
        else if (!wcscmp(a, L"--adv-report")) adv_report = true;
        else if (!wcscmp(a, L"--no-adv-sensors")) adv_set_enabled(false);
        else if (!wcscmp(a, L"--sysinfo")) return print_report(has_val && argv[i + 1][0] != L'-' ? argv[i + 1] : nullptr);
        else if (!wcscmp(a, L"--write-through")) params.disk_write_through = true;
        else if (!wcscmp(a, L"--capture") && has_val) { capture = true; strncpy_s(capture_target, val, _TRUNCATE); ++i; }
        else if (!wcscmp(a, L"--seconds") && has_val) {     // length of a sensor log or a frame capture
            capture_seconds = atof(val);
            log_seconds = (u32)atoi(val);
            ++i;
        }
        else if (!wcscmp(a, L"--capture-etl") && has_val) { capture = true; capture_etl = argv[++i]; }
        else if (!wcscmp(a, L"--capture-selftest")) capture_check = true;
        else if (!wcscmp(a, L"--score")) {
            score = true;
            while (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2) != 0 && score_file_count < ARRAY_COUNT(score_files))
                score_files[score_file_count++] = argv[++i];
        }
        else if (!wcscmp(a, L"--score-write")) score_write = true;
        else if (!wcscmp(a, L"--no-history")) no_history = true;
        else if (!wcscmp(a, L"--html") && has_val) html_path = argv[++i];
        else if (!wcscmp(a, L"--yes")) yes = true;
        else if (!wcscmp(a, L"--disk-buffered")) params.disk.buffered = true;
        else if (!wcscmp(a, L"--disk-profile") && has_val) {
            const DiskProfile* dp = disk_profile_find(val);
            if (!dp) { fprintf(stderr, "unknown disk profile '%s' (default, nvme, peak, realworld, demo)\n", val); return 3; }
            if (from_preset) memset(selected, 0, sizeof(selected));
            from_preset = false;
            any_run = true;
            bool prof[MAX_BENCHES] = {};
            disk_profile_apply(dp, prof);
            for (u32 k = 0; k < g_bench_count; ++k) selected[k] |= prof[k];
            ++i;
        }
        else if (!wcscmp(a, L"--disk-mode") && has_val) {
            if (!disk_mode_parse(val, &params.disk.mode)) { fprintf(stderr, "unknown disk mode '%s' (rw, read, write, mix)\n", val); return 3; }
            ++i;
        }
        else if (!wcscmp(a, L"--disk-mix") && has_val) { const int v = atoi(val); params.disk.mix_read_pct = (u8)(v < 0 ? 0 : v > 100 ? 100 : v); ++i; }
        else if (!wcscmp(a, L"--disk-data") && has_val) {
            if (!_stricmp(val, "zero")) params.disk.data = DISK_DATA_ZERO;
            else if (!_stricmp(val, "random")) params.disk.data = DISK_DATA_RANDOM;
            else { fprintf(stderr, "unknown disk data '%s' (random, zero)\n", val); return 3; }
            ++i;
        }
        else if (!wcscmp(a, L"--disk-folder") && has_val) wcsncpy_s(params.disk.folder, argv[++i], _TRUNCATE);
        else if (!wcscmp(a, L"--disk-interval") && has_val) { params.disk.interval_ms = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--sustained-gib") && has_val) { params.disk.sustained_gib = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--disk-custom") && has_val) {
            char* ctx = nullptr;
            u32 n = 0;
            for (char* tok = strtok_s(val, ";", &ctx); tok && n < DISK_CUSTOM_ROWS; tok = strtok_s(nullptr, ";", &ctx), ++n)
                if (!disk_row_parse(tok, &params.disk.custom[n])) {
                    fprintf(stderr, "bad custom disk row '%s' (e.g. RND4K Q32T4 R70)\n", tok);
                    return 3;
                }
            ++i;
        }
        else if (!wcscmp(a, L"--run") && has_val) { if (from_preset) memset(selected, 0, sizeof(selected)); from_preset = false; any_run = true; if (!select_patterns(val, selected)) return 3; ++i; }
        else if (!wcscmp(a, L"--out") && has_val) out_path = argv[++i];
        else if (!wcscmp(a, L"--compare") && has_val) compare_path = argv[++i];
        else if (!wcscmp(a, L"--log-sensors") && has_val) sensor_log = argv[++i];
        else if (!wcscmp(a, L"--interval") && has_val) { log_interval = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--sensors") && has_val) { strncpy_s(sensor_patterns, val, _TRUNCATE); ++i; }
        else if (!wcscmp(a, L"--list-sensors")) list_channels = true;
        else if (!wcscmp(a, L"--duration") && has_val) { params.duration_ms = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--threads") && has_val) { params.thread_count = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--gpu") && has_val) {     // "1" or "0,1"
            u32 mask = 0;
            for (const char* c = val; *c;) {
                if (*c < '0' || *c > '9') { ++c; continue; }
                char* end;
                const u32 idx = (u32)strtoul(c, &end, 10);
                if (idx < 32) mask |= 1u << idx;
                c = end;
            }
            if (mask) {
                params.gpu_adapter = (u32)__builtin_ctz(mask);
                params.gpu_adapters = (mask & (mask - 1)) ? mask : 0;
            }
            ++i;
        }
        else if (!wcscmp(a, L"--gpu-present") && has_val) {
            if (!strcmp(val, "offscreen")) params.gpu_onscreen = false;
            else if (!strcmp(val, "windowed") || !strcmp(val, "fullscreen")) { params.gpu_onscreen = true; params.gpu_fullscreen = val[0] == 'f'; }
            else { fprintf(stderr, "unknown --gpu-present mode '%s' (offscreen, windowed, fullscreen)\n", val); return 3; }
            ++i;
        }
        else if (!wcscmp(a, L"--disk-drives") && has_val) {
            params.disk_drives = 0;
            for (const char* d = val; *d; ++d)
                if (towupper(*d) >= L'A' && towupper(*d) <= L'Z') params.disk_drives |= 1u << (towupper(*d) - L'A');
            ++i;
        }
        else if (!wcscmp(a, L"--disk-size") && has_val) { params.disk_file_bytes = (u64)atoi(val) * MiB; ++i; }
        else if (!wcscmp(a, L"--passes") && has_val) { params.disk_passes = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--repeat") && has_val) { params.repeat = (u32)atoi(val); ++i; }
        else if (!wcscmp(a, L"--sustain") && has_val) {
            const f64 s = atof(val) * 60.0;
            params.sustain_s = s <= 0.0 ? 0 : s >= MAX_SUSTAIN_S ? MAX_SUSTAIN_S : (u32)(s + 0.5);
            ++i;
        }
        else if (!wcscmp(a, L"--preset") && has_val) {
            const RunPreset* pr = preset_find(val);
            if (!pr) { fprintf(stderr, "unknown preset '%s' (quick, standard, extended, sustained, score)\n", val); return 3; }
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

    if (health) return print_health(hide_serial);
    if (list_channels) return list_sensors();
    if (sensor_log && !any_run) return log_sensors_only(sensor_log, log_interval, log_seconds, sensor_patterns);
    if (capture_check) {     // --out also writes the synthetic capture (a test file for the compare window)
        char report[1024];
        const bool pass = capture_selftest(report, sizeof(report));
        printf("%s\n", report);
        if (out_path && !capture_write_toon(out_path, capture_result())) return 1;
        return pass ? 0 : 1;
    }
    if (capture) {
        if (!capture_etl && !capture_target[0]) { fprintf(stderr, "--capture needs a process name, pid or *\n"); return 3; }
        if (capture_seconds < 1.0) capture_seconds = 1.0;
        return run_capture(capture_target, capture_seconds, capture_etl, out_path);
    }
    if (score_file_count) {     // score of saved files: no run
        int rc = 0;
        for (u32 f = 0; f < score_file_count; ++f) {
            ToonReader r;
            char err[160];
            char* buf = toon_read_file(score_files[f], &r, err, sizeof(err));
            ScoreResult sr;
            if (!buf || !score_from_text(r.p, (u32)(r.end - r.p), &sr)) {
                fwprintf(stderr, L"%s: ", score_files[f]);
                fprintf(stderr, "%s\n", buf ? "not an EzPcBench results file" : err);
                toon_free_file(buf);
                rc = 3;
                continue;
            }
            const IntegrityState integ = integrity_check(r.p, (u32)(r.end - r.p));
            toon_free_file(buf);
            wprintf(L"%s\n", score_files[f]);
            printf("Integrity: %s\n", integrity_name(integ));
            score_print(stdout, &sr);
            if (score_write) {
                if (score_rewrite_file(score_files[f], err, sizeof(err))) printf("Score block written.\n");
                else { fprintf(stderr, "Cannot write the score block: %s\n", err); rc = 1; }
            }
            printf("\n");
        }
        return rc;
    }

    if (elevate && !g_sys.elevated) {
        if (relaunch_elevated(L"--pause")) return 0;
        fprintf(stderr, "Elevation declined or failed; continuing without admin rights.\n");
    }

    adv_init();         // PawnIO advanced sensors: no-op unless installed (and not --no-adv-sensors)
    if (adv_report) {
        adv_print_report();
        return 0;
    }

    if (!any_run)
        for (u32 i = 0; i < g_bench_count; ++i) selected[i] = !(g_benches[i].flags & BENCH_DEFAULT_OFF);
    for (u32 i = 0; i < g_bench_count && !yes; ++i) {
        if (!selected[i] || !(g_benches[i].flags & BENCH_CONFIRM)) continue;
        char note[256];
        disk_confirm_note(&params, i, note, sizeof(note));
        fprintf(stderr, "skipping %s: %s Add --yes to run it.\n", g_benches[i].cli_id, note);
        selected[i] = false;
    }

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
    wchar_t hist[MAX_PATH];
    if (!no_history && history_save(hist)) wprintf(L"History: %s\n", hist);
    if (score) {
        ScoreResult sr;
        score_current(&sr);
        printf("\n");
        score_print(stdout, &sr);
    }
    if (html_path) {
        char err[160];
        compare_clear();
        if (compare_add_current(err, sizeof(err)) && report_write_html(html_path)) wprintf(L"Wrote %s\n", html_path);
        else { fwprintf(stderr, L"Failed to write %s\n", html_path); exit_code = 1; }
    }
    if (pause) {
        printf("Press Enter to exit...");
        getchar();
    }
    return exit_code;
}
