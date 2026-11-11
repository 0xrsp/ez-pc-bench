// CLI stability mode: ez-pc-bench-cli --stress <tests> [--minutes N | --seconds N] [options]. Prints a status line
// every second, then the report; writes the TOON report with --out. Exit code 0 = pass, 1 = errors / WHEA events,
// 2 = stopped early (limit, Ctrl+C) or a test couldn't run, 3 = usage.
#include "app/stability_cli.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/whea.h"
#include "stability/stability.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

void stability_cli_usage() {
    printf(
        "Stability tester (separate from the benchmarks):\n"
        "  --stress <tests>       Comma-separated: cpu, cache, cores, mem, vram, gpu, power, disk, all (all but\n"
        "                         power), combined (cpu + gpu together)\n"
        "  --minutes <n>          Time per test (together: total). Default 10; 0 = until Ctrl+C\n"
        "  --seconds <n>          Same in seconds\n"
        "  --together             Run the tests at the same time instead of one after another\n"
        "  --cpu-kernel <k>       mixed, int, sse, avx2, avx512, large (default mixed)\n"
        "  --core-seconds <n>     Per-core cycling: time on each core (default 60)\n"
        "  --mem-mib <n>          Memory test size (default 70%% of available RAM, keeps 2 GiB free)\n"
        "  --vram-mib <n>         VRAM test size (default 85%% of free video memory)\n"
        "  --gpu <index>          GPU adapter (default: most powerful)\n"
        "  --disk-drive <letter>  Disk verify target (default C)\n"
        "  --disk-mib <n>         Disk verify file size (default 256)\n"
        "  --max-cpu-temp <C>     Stop at this CPU temperature (default 95, 0 = off; ACPI zone, best effort)\n"
        "  --max-gpu-temp <C>     Stop at this GPU temperature (default 87, 0 = off)\n"
        "  --max-drive-temp <C>   Stop at this drive temperature (default 70, 0 = off)\n"
        "  --stop-on-error        Stop at the first error\n"
        "  --inject               Self-test: each test injects one fault (proves error detection works)\n"
        "  --out <file.toon.txt>  Write the stability report\n");
}

static BOOL WINAPI on_ctrl(DWORD type) {
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && stab_is_running()) {
        stab_stop("stopped by user");
        return TRUE;
    }
    return FALSE;
}

static void fmt_time(char* buf, u32 size, f64 s) {
    const u32 t = (u32)s;
    snprintf(buf, size, "%u:%02u:%02u", t / 3600, t / 60 % 60, t % 60);
}

static void print_status() {
    StabSensors* s = (StabSensors*)malloc(sizeof(StabSensors));
    if (!s) return;
    stab_sensors(s);
    char el[16];
    fmt_time(el, sizeof(el), stab_elapsed());
    const i32 cur = g_stab.current.load();
    char what[96] = "";
    if (cur >= 0) {
        const char* ph = g_stab.tests[cur].phase.load();
        snprintf(what, sizeof(what), "%s%s%s", g_stab_tests[cur].id, ph ? ": " : "", ph ? ph : "");
    } else {
        snprintf(what, sizeof(what), "together");
    }
    char line[256];
    int n = snprintf(line, sizeof(line), "%s %-30.30s err %llu", el, what, stab_total_errors());
    if (!isnan(s->now[SH_CPU_MHZ])) n += snprintf(line + n, sizeof(line) - n, " | CPU %.2f GHz %.0f%%", s->now[SH_CPU_MHZ] / 1000.0f, s->now[SH_CPU_UTIL]);
    if (!isnan(s->now[SH_CPU_TEMP])) n += snprintf(line + n, sizeof(line) - n, " %.0fC", s->now[SH_CPU_TEMP]);
    if (!isnan(s->now[SH_GPU_TEMP])) n += snprintf(line + n, sizeof(line) - n, " | GPU %.0fC", s->now[SH_GPU_TEMP]);
    if (!isnan(s->now[SH_GPU_UTIL])) n += snprintf(line + n, sizeof(line) - n, " %.0f%%", s->now[SH_GPU_UTIL]);
    if (!isnan(s->now[SH_DRIVE_TEMP])) n += snprintf(line + n, sizeof(line) - n, " | drive %.0fC", s->now[SH_DRIVE_TEMP]);
    snprintf(line + n, sizeof(line) - n, " | WHEA %u", g_stab.whea_corrected + g_stab.whea_uncorrected);
    printf("\r%-118.118s", line);
    fflush(stdout);
    free(s);
}

static void print_report() {
    printf("\r%-118s\r", "");
    char why[256];
    const char* verdict = stab_verdict(why, sizeof(why));
    printf("\nStability: %s - %s (%.1f s)\n", verdict, why, stab_elapsed());
    if (g_stab.stop_reason[0]) printf("Stop reason: %s\n", g_stab.stop_reason);
    printf("%-8s %-17s %-8s %8s %8s %12s  %s\n", "test", "name", "status", "seconds", "errors", "first err s", "work / notes");
    for (u32 t = 0; t < STAB_COUNT; ++t) {
        const StabTestState* s = &g_stab.tests[t];
        if (!g_stab.cfg.run[t]) continue;
        const f64 secs = s->end_t > s->start_t ? s->end_t - s->start_t : 0.0;
        const f64 fe = s->first_error_s.load();
        char fes[16] = "-";
        if (fe >= 0.0) snprintf(fes, sizeof(fes), "%.2f", fe);
        printf("%-8s %-17s %-8s %8.1f %8llu %12s  %.3f %s%s%s\n", g_stab_tests[t].id, g_stab_tests[t].name,
               stab_status_name((StabStatus)s->status.load()), secs, s->errors.load(), fes, s->work.load(),
               g_stab_tests[t].work_unit, s->note[0] ? "; " : "", s->note);
        if (t == STAB_CPU || t == STAB_CACHE || t == STAB_CORES || t == STAB_MEM) {
            bool any = false;
            for (u32 lp = 0; lp < g_topo.lp_count && lp < STAB_MAX_LP; ++lp) any |= s->lp_errors[lp].load() != 0;
            if (any) {
                printf("         errors by logical processor:");
                for (u32 lp = 0; lp < g_topo.lp_count && lp < STAB_MAX_LP; ++lp)
                    if (s->lp_errors[lp].load()) printf(" LP%u (core %u): %u", lp, g_topo.core[lp], s->lp_errors[lp].load());
                printf("\n");
            }
            if (t == STAB_CORES) {
                printf("         seconds per core:");
                for (u32 lp = 0; lp < g_topo.lp_count && lp < STAB_MAX_LP; ++lp)
                    if (s->lp_seconds[lp].load() > 0.0f) printf(" c%u %.1f", g_topo.core[lp], s->lp_seconds[lp].load());
                printf("\n");
            }
        }
        bool chunks = false;
        for (u32 c = 0; c < STAB_MAX_CHUNKS; ++c) chunks |= s->chunk_errors[c].load() != 0;
        if (chunks && t != STAB_GPU) {
            printf("         errors by chunk (%llu MiB each):", s->chunk_bytes / MiB);
            for (u32 c = 0; c < STAB_MAX_CHUNKS; ++c)
                if (s->chunk_errors[c].load()) printf(" #%u: %u", c, s->chunk_errors[c].load());
            printf("\n");
        }
    }
    StabSensors* s = (StabSensors*)malloc(sizeof(StabSensors));
    if (s) {
        stab_sensors(s);
        printf("Sensors (avg / max): CPU %.0f / %.0f MHz, load %.0f%%, temp %.1f / %.1f C | GPU %.0f / %.0f MHz, "
               "temp %.1f / %.1f C, power %.0f%% | drive %.1f / %.1f C  (nan = not reported)\n",
               s->avg[SH_CPU_MHZ], s->max[SH_CPU_MHZ], s->avg[SH_CPU_UTIL], s->avg[SH_CPU_TEMP], s->max[SH_CPU_TEMP],
               s->avg[SH_GPU_MHZ], s->max[SH_GPU_MHZ], s->avg[SH_GPU_TEMP], s->max[SH_GPU_TEMP], s->avg[SH_GPU_POWER],
               s->avg[SH_DRIVE_TEMP], s->max[SH_DRIVE_TEMP]);
        free(s);
    }
    printf("WHEA during the run: %u corrected, %u uncorrected%s\n", g_stab.whea_corrected, g_stab.whea_uncorrected,
           g_stab.whea_active ? "" : " (not monitored: the System event log couldn't be subscribed)");
    StabError log[STAB_MAX_LOG];
    const u32 n = stab_error_log(log, STAB_MAX_LOG);
    for (u32 i = 0; i < n && i < 16; ++i) {
        const StabError* e = &log[i];
        printf("  error %2u  %8.3f s  %-6s %-26s lp %3d chunk %3d  at 0x%llX  expected 0x%016llX  actual 0x%016llX  "
               "bits 0x%llX\n", i + 1, e->t, g_stab_tests[e->test].id, e->what, e->lp, e->chunk, e->address, e->expected,
               e->actual, e->expected ^ e->actual);
    }
    if (n > 16) printf("  ... %u errors logged in total (first %u in the report file)\n", n, STAB_MAX_LOG);
}

static void to_utf8(const wchar_t* w, char* out, int size) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, nullptr, nullptr); }

int stability_cli(int argc, wchar_t** argv) {
    StabConfig cfg;
    cfg.gpu_adapter = sysinfo_default_gpu();
    const wchar_t* out = nullptr;
    bool have_tests = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        const bool has_val = i + 1 < argc;
        char v[256] = {};
        if (has_val) to_utf8(argv[i + 1], v, sizeof(v));
        if (!wcscmp(a, L"--stress") && has_val) {
            char err[128];
            const bool together = cfg.together;
            if (!stab_parse_ids(v, &cfg, err, sizeof(err))) { fprintf(stderr, "%s\n", err); return 3; }
            cfg.together |= together;
            have_tests = true;
            ++i;
        }
        else if (!wcscmp(a, L"--minutes") && has_val) { cfg.seconds = (u32)(atof(v) * 60.0); ++i; }
        else if (!wcscmp(a, L"--seconds") && has_val) { cfg.seconds = (u32)atoi(v); ++i; }
        else if (!wcscmp(a, L"--together")) cfg.together = true;
        else if (!wcscmp(a, L"--cpu-kernel") && has_val) {
            bool ok = false;
            for (u32 k = 0; k < CK_COUNT; ++k)
                if (!strcmp(v, g_cpu_kernel_ids[k])) { cfg.cpu_kernel = (CpuKernel)k; ok = true; }
            if (!ok) { fprintf(stderr, "unknown CPU kernel '%s'\n", v); return 3; }
            ++i;
        }
        else if (!wcscmp(a, L"--core-seconds") && has_val) { cfg.core_seconds = (u32)atoi(v); ++i; }
        else if (!wcscmp(a, L"--mem-mib") && has_val) { cfg.mem_bytes = (u64)atoll(v) * MiB; ++i; }
        else if (!wcscmp(a, L"--vram-mib") && has_val) { cfg.vram_bytes = (u64)atoll(v) * MiB; ++i; }
        else if (!wcscmp(a, L"--gpu") && has_val) { cfg.gpu_adapter = (u32)atoi(v); ++i; }
        else if (!wcscmp(a, L"--disk-drive") && has_val) { cfg.disk_drive = (char)towupper((wchar_t)v[0]); ++i; }
        else if (!wcscmp(a, L"--disk-mib") && has_val) { cfg.disk_bytes = (u64)atoll(v) * MiB; ++i; }
        else if (!wcscmp(a, L"--max-cpu-temp") && has_val) { cfg.max_cpu_c = (f32)atof(v); ++i; }
        else if (!wcscmp(a, L"--max-gpu-temp") && has_val) { cfg.max_gpu_c = (f32)atof(v); ++i; }
        else if (!wcscmp(a, L"--max-drive-temp") && has_val) { cfg.max_drive_c = (f32)atof(v); ++i; }
        else if (!wcscmp(a, L"--stop-on-error")) cfg.stop_on_error = true;
        else if (!wcscmp(a, L"--inject")) cfg.inject = true;
        else if (!wcscmp(a, L"--out") && has_val) out = argv[++i];
        else if (!wcscmp(a, L"--whea-history")) {      // dev: proves the WHEA-Logger query works
            u32 c = 0, u = 0;
            if (!whea_history(&c, &u)) { fprintf(stderr, "cannot read the System event log\n"); return 1; }
            printf("WHEA-Logger events in the System log: %u corrected, %u uncorrected\n", c, u);
            return 0;
        }
        else { stability_cli_usage(); return 3; }
    }
    if (!have_tests) { stability_cli_usage(); return 3; }

    printf("CPU   %s, %uC/%uT\n", g_sys.cpu.brand, g_sys.cpu.cores, g_sys.cpu.threads);
    if (cfg.gpu_adapter < g_sys.gpu_count) printf("GPU   %s\n", g_sys.gpus[cfg.gpu_adapter].name);
    printf("Power %s\n", g_sys.power.on_ac ? "AC" : "battery");
    printf("Tests:");
    for (u32 t = 0; t < STAB_COUNT; ++t)
        if (cfg.run[t]) printf(" %s", g_stab_tests[t].id);
    if (cfg.seconds) printf(", %u s %s", cfg.seconds, cfg.together ? "together" : "each");
    else printf(", until Ctrl+C%s", cfg.together ? ", together" : "");
    printf("%s\n", cfg.inject ? "  [SELF-TEST: injecting one fault per test]" : "");
    if (cfg.run[STAB_MEM])
        printf("Note: the memory test runs inside Windows and only reaches part of the RAM; MemTest86 (bootable) "
               "tests nearly all of it.\n");
    if (cfg.run[STAB_POWER])
        printf("Note: GPU max power is an unchecked maximum-power load; it stops at the GPU temperature limit.\n");
    printf("\n");

    SetConsoleCtrlHandler(on_ctrl, TRUE);
    char err[160];
    if (!stab_start(&cfg, err, sizeof(err))) { fprintf(stderr, "%s\n", err); return 3; }
    while (stab_is_running()) {
        print_status();
        Sleep(1000);
    }
    stab_wait();
    print_report();
    int code = 0;
    char why[256];
    const char* verdict = stab_verdict(why, sizeof(why));
    if (!strcmp(verdict, "fail")) code = 1;
    else if (strcmp(verdict, "pass") != 0) code = 2;
    if (out) {
        if (stab_write_toon(out)) wprintf(L"Wrote %s\n", out);
        else { fwprintf(stderr, L"Failed to write %s\n", out); if (!code) code = 1; }
    }
    return code;
}
