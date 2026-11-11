// Sustained write / SLC cache test (FUTURE.md 2) with latency over time (2.1).
// Writes up to DiskOptions::sustained_gib (at most 80 % of the free space) in one go into fresh files: 4 threads,
// each extending its own file with 1 MiB unbuffered writes. Extending writes are synchronous on NTFS even with
// OVERLAPPED, so parallel files are how we keep several requests in flight. Every write is logged (completion
// time and latency) into memory allocated before the loop. Afterwards:
//   - 100 ms windows of the log give MB/s (for the SLC cache estimate) and the p99 / max latency per window;
//   - SUS_POINTS points at fixed GB marks give the series: MB/s between two marks, the worst 100 ms p99 / max in
//     that stretch, and the drive temperature (sampled once a second). Fixed marks keep repeats and compares aligned.
#include "disk/disk_internal.h"
#include "core/perf.h"
#include "core/timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr u32 SUS_THREADS = 4;
constexpr u32 SUS_BLOCK = 1024 * 1024;
constexpr f64 SUS_WINDOW_S = 0.1;
constexpr u32 SUS_POINTS = 100;
constexpr u32 SUS_TEMPS = 8192;

struct SusEntry {
    u64 end;            // QPC tick at completion
    u32 lat;            // ticks
    u32 pad;
};

struct alignas(64) SusWriter {
    HANDLE file, thread;
    u8* buf;
    u64 blocks;                     // this thread's share
    SusEntry* log;                  // `blocks` entries
    std::atomic<u64> done;          // blocks written
    std::atomic<bool>* stop;
    DWORD error;
};
static SusWriter s_writers[SUS_THREADS];

struct SusWindow {
    f64 mbps, p99_ms, max_ms;
    u64 bytes_end;      // total written at the end of the window
};

struct SusRun {
    SusEntry* log;      // sorted by completion time
    u64 n, planned;     // entries written / planned
    u64 start;          // QPC tick at start
    f64 seconds;
    SusWindow* win;
    u32 nwin;
    f32 temps[SUS_TEMPS];
    f64 temp_t[SUS_TEMPS];      // seconds since start
    u32 ntemps;
};
static SusRun s_run;

static DWORD WINAPI sus_thread(void* arg) {
    SusWriter* w = (SusWriter*)arg;
    perf_setup_worker_thread();
    for (u64 i = 0; i < w->blocks && !w->stop->load(std::memory_order_relaxed); ++i) {
        DWORD done = 0;
        const u64 t0 = timer_ticks();
        if (!WriteFile(w->file, w->buf, SUS_BLOCK, &done, nullptr) || done != SUS_BLOCK) {
            w->error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            break;
        }
        const u64 t1 = timer_ticks();
        w->log[i] = { t1, (u32)(t1 - t0 < 0xFFFFFFFFull ? t1 - t0 : 0xFFFFFFFFull), 0 };
        w->done.store(i + 1, std::memory_order_relaxed);
    }
    return 0;
}

static int cmp_entry(const void* a, const void* b) {
    const u64 x = ((const SusEntry*)a)->end, y = ((const SusEntry*)b)->end;
    return x < y ? -1 : x > y;
}
static int cmp_u32(const void* a, const void* b) {
    const u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

static void close_writers() {
    for (u32 k = 0; k < SUS_THREADS; ++k) {
        SusWriter* w = &s_writers[k];
        if (w->thread) { WaitForSingleObject(w->thread, INFINITE); CloseHandle(w->thread); }
        if (w->file) CloseHandle(w->file);      // DELETE_ON_CLOSE removes the file
        w->thread = w->file = nullptr;
    }
}

// Creates the files and threads (suspended). False with `error` set; close_writers cleans up either way.
static bool open_writers(const DiskTarget* t, u64 blocks_per_thread, u8* bufs, std::atomic<bool>* stop, char* error,
                         u32 error_size) {
    const DWORD flags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                        (g_disk.write_through ? FILE_FLAG_WRITE_THROUGH : 0);
    for (u32 k = 0; k < SUS_THREADS; ++k) {
        SusWriter* w = &s_writers[k];
        w->file = w->thread = nullptr;
        w->buf = bufs + (u64)k * SUS_BLOCK;
        fill_pattern(w->buf, SUS_BLOCK, disk_zero_pct(), timer_ticks() + k);
        w->blocks = blocks_per_thread;
        w->log = s_run.log + k * blocks_per_thread;
        w->done.store(0);
        w->stop = stop;
        w->error = 0;
    }
    for (u32 k = 0; k < SUS_THREADS; ++k) {
        SusWriter* w = &s_writers[k];
        wchar_t path[MAX_PATH], name[32];
        swprintf(name, 32, L"ezb_sustained_%u.tmp", k);
        if (!disk_work_path(t, name, path)) { snprintf(error, error_size, "no work folder"); return false; }
        w->file = CreateFileW(path, GENERIC_WRITE | DELETE, SHARE_ALL, nullptr, CREATE_ALWAYS, flags, nullptr);
        if (w->file == INVALID_HANDLE_VALUE) {
            w->file = nullptr;
            snprintf(error, error_size, "cannot create %ls (error %lu)", name, GetLastError());
            return false;
        }
        // Reserve the space up front (no zero fill: the valid data length stays at 0 until we write).
        LARGE_INTEGER size, zero = {};
        size.QuadPart = (LONGLONG)(blocks_per_thread * SUS_BLOCK);
        if (!SetFilePointerEx(w->file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(w->file) ||
            !SetFilePointerEx(w->file, zero, nullptr, FILE_BEGIN)) {
            snprintf(error, error_size, "cannot size %ls (error %lu)", name, GetLastError());
            return false;
        }
        w->thread = CreateThread(nullptr, 0, sus_thread, w, CREATE_SUSPENDED, nullptr);
        if (!w->thread) { snprintf(error, error_size, "CreateThread failed"); return false; }
    }
    return true;
}

// Writes `cap` bytes on one target into s_run. False on an error (message in `error`).
static bool sustained_write(const DiskTarget* t, u64 cap, BenchLive* live, const CancelFlag* cancel, f32 from, f32 to,
                            char* error, u32 error_size) {
    const u64 per_thread = cap / SUS_THREADS / SUS_BLOCK;
    s_run.planned = per_thread * SUS_THREADS;
    s_run.log = (SusEntry*)disk_alloc(s_run.planned * sizeof(SusEntry));
    u8* bufs = (u8*)disk_alloc((u64)SUS_BLOCK * SUS_THREADS);
    std::atomic<bool> stop(false);
    bool ok = s_run.log && bufs;
    if (!ok) snprintf(error, error_size, "out of memory");
    ok = ok && open_writers(t, per_thread, bufs, &stop, error, error_size);
    if (!ok) {
        stop.store(true);
        for (u32 k = 0; k < SUS_THREADS; ++k)
            if (s_writers[k].thread) ResumeThread(s_writers[k].thread);
        close_writers();
        disk_free(bufs);
        return false;
    }

    s_run.ntemps = 0;
    HANDLE hs[SUS_THREADS];
    for (u32 k = 0; k < SUS_THREADS; ++k) hs[k] = s_writers[k].thread;
    s_run.start = timer_ticks();
    const f64 t0 = timer_now();
    for (u32 k = 0; k < SUS_THREADS; ++k) ResumeThread(s_writers[k].thread);
    f64 last_t = t0, last_temp = -10.0;
    u64 last_blocks = 0;
    for (;;) {
        const bool done = WaitForMultipleObjects(SUS_THREADS, hs, TRUE, 100) == WAIT_OBJECT_0;
        const f64 now = timer_now();
        u64 blocks = 0;
        for (u32 k = 0; k < SUS_THREADS; ++k) blocks += s_writers[k].done.load(std::memory_order_relaxed);
        if (now > last_t) live_sample(live, (f64)(blocks - last_blocks) * SUS_BLOCK / (now - last_t) / 1e6);
        last_t = now;
        last_blocks = blocks;
        live_progress(live, from + (to - from) * (f32)((f64)blocks / s_run.planned));
        if (now - last_temp >= 1.0 && s_run.ntemps < SUS_TEMPS) {
            f32 c;
            if (disk_temperature(t, &c)) {
                s_run.temps[s_run.ntemps] = c;
                s_run.temp_t[s_run.ntemps++] = now - t0;
            }
            last_temp = now;
        }
        if (done) break;
        bool failed = cancelled(cancel);
        for (u32 k = 0; k < SUS_THREADS; ++k) failed |= s_writers[k].error != 0;
        if (failed) stop.store(true);
    }
    s_run.seconds = (timer_ticks() - s_run.start) * timer_tick_seconds();
    for (u32 k = 0; k < SUS_THREADS && ok; ++k)
        if (s_writers[k].error) { snprintf(error, error_size, "WriteFile failed (error %lu)", s_writers[k].error); ok = false; }
    // Compact the per-thread logs, then sort by completion time: entry k completes the (k+1)-th MiB written.
    s_run.n = 0;
    for (u32 k = 0; k < SUS_THREADS; ++k) {
        const u64 got = s_writers[k].done.load();
        memmove(s_run.log + s_run.n, s_writers[k].log, got * sizeof(SusEntry));
        s_run.n += got;
    }
    close_writers();
    disk_free(bufs);
    qsort(s_run.log, (size_t)s_run.n, sizeof(SusEntry), cmp_entry);
    return ok;
}

// 100 ms windows: MB/s, p99, max. Needs s_run.log sorted.
static bool make_windows() {
    const u64 window_ticks = (u64)(SUS_WINDOW_S / timer_tick_seconds());
    const u64 span = s_run.n ? s_run.log[s_run.n - 1].end - s_run.start : 0;
    const u32 max_windows = (u32)(span / window_ticks + 1);
    s_run.win = (SusWindow*)disk_alloc((u64)max_windows * sizeof(SusWindow));
    u32* lats = (u32*)disk_alloc(s_run.n * sizeof(u32) + 4);
    s_run.nwin = 0;
    if (!s_run.win || !lats) { disk_free(lats); return false; }
    u64 i = 0;
    for (u32 w = 0; w < max_windows && i < s_run.n; ++w) {
        const u64 w_end = s_run.start + (w + 1) * window_ticks;
        u32 m = 0;
        for (; i < s_run.n && s_run.log[i].end < w_end; ++i) lats[m++] = s_run.log[i].lat;
        SusWindow* sw = &s_run.win[s_run.nwin++];
        sw->bytes_end = i * SUS_BLOCK;
        sw->mbps = (f64)m * SUS_BLOCK / SUS_WINDOW_S / 1e6;
        sw->p99_ms = sw->max_ms = 0.0;
        if (m) {
            qsort(lats, m, sizeof(u32), cmp_u32);
            sw->p99_ms = ticks_us(lats[(u32)(0.99 * (m - 1) + 0.5)]) / 1000.0;
            sw->max_ms = ticks_us(lats[m - 1]) / 1000.0;
        }
    }
    disk_free(lats);
    return true;
}

// In-cache speed: median of the 1 s moving average over the first 10 % of windows. The cache counts as exhausted
// where the moving average falls below 60 % of it and stays there for 2 s. Returns the window index or -1.
static i32 find_cache_end(f64* in_cache) {
    constexpr u32 AVG = 10, HOLD = 20;
    static f64 smooth[1 << 16], sorted[1 << 16];
    const u32 ns = s_run.nwin < ARRAY_COUNT(smooth) ? s_run.nwin : ARRAY_COUNT(smooth);
    f64 acc = 0.0;
    for (u32 w = 0; w < ns; ++w) {
        acc += s_run.win[w].mbps;
        if (w >= AVG) acc -= s_run.win[w - AVG].mbps;
        smooth[w] = acc / (w + 1 < AVG ? w + 1 : AVG);
    }
    const u32 head = ns / 10 > AVG ? ns / 10 : ns < AVG ? ns : AVG;
    memcpy(sorted, smooth, head * sizeof(f64));
    for (u32 a = 1; a < head; ++a)
        for (u32 b = a; b > 0 && sorted[b - 1] > sorted[b]; --b) { const f64 x = sorted[b]; sorted[b] = sorted[b - 1]; sorted[b - 1] = x; }
    *in_cache = head ? sorted[head / 2] : 0.0;
    for (u32 w = head; w + HOLD < ns; ++w) {
        if (smooth[w] >= 0.6 * *in_cache) continue;
        bool stays = true;
        for (u32 k = w; k < w + HOLD && stays; ++k) stays = smooth[k] < 0.6 * *in_cache;
        if (stays) return (i32)w;
    }
    return -1;
}

static void add_series(BenchResult* out, const char* label) {
    const char* names[4] = { dname("%s Write", label), dname("%s p99 latency (100 ms)", label),
                             dname("%s Max latency (100 ms)", label), dname("%s Temperature", label) };
    const f64 tick_s = timer_tick_seconds();
    for (u32 line = 0; line < 4; ++line) {
        if (line == 3 && !s_run.ntemps) continue;
        f64 t_prev = 0.0;
        u64 b_prev = 0;
        for (u32 j = 1; j <= SUS_POINTS; ++j) {
            const u64 idx = (s_run.planned * j + SUS_POINTS - 1) / SUS_POINTS;     // entries up to this mark
            const u64 e = (idx < s_run.n ? idx : s_run.n) - 1;
            const f64 t_j = (s_run.log[e].end - s_run.start) * tick_s;
            const f64 x = floor((f64)idx * SUS_BLOCK / 1e9 * 1000.0 + 0.5) / 1000.0;
            f64 v = 0.0;
            if (line == 0) {
                v = t_j > t_prev ? (f64)(e + 1 - b_prev) * SUS_BLOCK / (t_j - t_prev) / 1e6 : 0.0;
            } else if (line < 3) {
                u32 w0 = (u32)(t_prev / SUS_WINDOW_S), w1 = (u32)(t_j / SUS_WINDOW_S);
                if (w1 >= s_run.nwin) w1 = s_run.nwin - 1;
                if (w0 > w1) w0 = w1;
                for (u32 w = w0; w <= w1; ++w) {
                    const f64 y = line == 1 ? s_run.win[w].p99_ms : s_run.win[w].max_ms;
                    v = y > v ? y : v;
                }
            } else {
                // Highest temperature sampled in this stretch, else the latest one before it.
                v = -1000.0;
                for (u32 k = 0; k < s_run.ntemps; ++k)
                    if ((s_run.temp_t[k] >= t_prev && s_run.temp_t[k] <= t_j) ||
                        (s_run.temp_t[k] < t_prev && (k + 1 == s_run.ntemps || s_run.temp_t[k + 1] > t_j)))
                        v = s_run.temps[k] > v ? s_run.temps[k] : v;
                if (v < -999.0) v = s_run.temps[0];
            }
            res_add(out, names[line], line == 0 ? "MB/s" : line == 3 ? "\xC2\xB0" "C" : "ms", v, x);
            t_prev = t_j;
            b_prev = e + 1;
        }
    }
}

void disk_sustained_write(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    if (g_disk.buffered) { res_skip(out, "needs unbuffered I/O (turn off the buffered option)"); return; }
    char error[128] = {};
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Written", "GB");
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        wchar_t probe[MAX_PATH];
        disk_work_path(t, L"", probe);      // creates the work folder
        ULARGE_INTEGER free_user = {};
        GetDiskFreeSpaceExW(t->work_dir, &free_user, nullptr, nullptr);
        const u64 room = free_user.QuadPart > GiB ? (free_user.QuadPart - GiB) / 10 * 8 : 0;
        u64 cap = (u64)(p->disk.sustained_gib ? p->disk.sustained_gib : 64) * GiB;
        if (cap > room) cap = room;
        if (cap < (u64)SUS_POINTS * SUS_BLOCK * SUS_THREADS) { res_fail(out, "%s not enough free space", t->label); return; }
        live_phase(live, dname("%s sustained write of %.1f GB", t->label, cap / 1e9), "MB/s");
        const bool ok = sustained_write(t, cap, live, cancel, (f32)i / g_disk.count, (f32)(i + 1) / g_disk.count, error,
                                        sizeof(error));
        const bool windows = ok && !cancelled(cancel) && s_run.n == s_run.planned && make_windows();
        if (windows) {
            f64 in_cache = 0.0;
            const i32 cache_end = find_cache_end(&in_cache);
            const f64 total_gb = s_run.n * (f64)SUS_BLOCK / 1e9;
            add_series(out, t->label);
            // Summary lines, one point each at the total written. "Cache drop at" is 0 when the speed never fell
            // below 60 % of the in-cache speed (cache larger than what was written, or no SLC cache).
            f64 after = 0.0, tmax = -1000.0;
            if (cache_end >= 0) {
                const f64 gb0 = s_run.win[cache_end].bytes_end / 1e9;
                after = (total_gb - gb0) * 1e3 / ((s_run.nwin - cache_end) * SUS_WINDOW_S);
            }
            for (u32 k = 0; k < s_run.ntemps; ++k) tmax = s_run.temps[k] > tmax ? s_run.temps[k] : tmax;
            const char* label = t->label;
            res_add(out, dname("%s Average write", label), "MB/s", total_gb * 1e3 / s_run.seconds, total_gb);
            res_add(out, dname("%s In-cache write", label), "MB/s", in_cache, total_gb);
            res_add(out, dname("%s Cache drop at", label), "GB", cache_end >= 0 ? s_run.win[cache_end].bytes_end / 1e9 : 0.0, total_gb);
            res_add(out, dname("%s After-cache write", label), "MB/s", after, total_gb);
            if (s_run.ntemps) res_add(out, dname("%s Max temperature", label), "\xC2\xB0" "C", tmax, total_gb);
        }
        disk_free(s_run.win);
        disk_free(s_run.log);
        s_run.win = nullptr;
        s_run.log = nullptr;
        if (!ok) { res_fail(out, "%s %s", t->label, error); return; }
        if (cancelled(cancel)) return;
        if (!windows) { res_fail(out, "%s out of memory", t->label); return; }
    }
}
