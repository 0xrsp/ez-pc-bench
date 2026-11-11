// Automatic run history (see history.h).
#include "core/history.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

void history_dir(wchar_t* dir) {
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    wcscat_s(dir, MAX_PATH, L"\\results");
    CreateDirectoryW(dir, nullptr);
    wcscat_s(dir, MAX_PATH, L"\\history");
    CreateDirectoryW(dir, nullptr);
}

bool history_save(wchar_t* path_out) {
    u32 done = 0;
    for (u32 i = 0; i < g_bench_count; ++i) done += g_status[i].load(std::memory_order_acquire) == ST_DONE;
    if (!done) return false;
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    const BenchParams* p = &g_run.params;
    char cpu[48], gpu[48];
    cpu_short_name(g_sys.cpu.brand, cpu, sizeof(cpu));
    gpu_short_name(p->gpu_adapter < g_sys.gpu_count ? g_sys.gpus[p->gpu_adapter].name : "", gpu, sizeof(gpu));
    char name[128];
    snprintf(name, sizeof(name), "%04d%02d%02d_%02d%02d%02d_%s_%s.toon.txt", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, cpu, gpu);
    wchar_t dir[MAX_PATH], wname[128], path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 128);
    history_dir(dir);
    swprintf(path, MAX_PATH, L"%s\\%s", dir, wname);
    if (path_out) wcscpy_s(path_out, MAX_PATH, path);
    return results_write_toon(path, p);
}
