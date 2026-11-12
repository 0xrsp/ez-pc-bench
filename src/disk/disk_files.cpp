// Tests through the normal file system API: small-file metadata latency (create / open / stat / delete) and
// AS SSD-style copy workloads (one ISO-like file, a "program" set of many small files, a "game" set of mixed
// sizes). Files live in the target's ezb_work folder and are deleted afterwards.
#include "disk/disk_internal.h"
#include "core/timer.h"
#include <stdio.h>
#include <string.h>

// ---- metadata latency ------------------------------------------------------------------------------------------

constexpr u32 META_FILES = 500;         // per round
constexpr u32 META_BYTES = 1024;

enum MetaOp { META_CREATE, META_OPEN, META_STAT, META_DELETE, META_OPS };
static const char* const META_NAME[META_OPS] = { "Create", "Open+read", "Stat", "Delete" };

static bool meta_round(const wchar_t* dir, const u8* data, LatHist* h, f64* busy, char* error, u32 error_size) {
    static wchar_t paths[META_FILES][MAX_PATH];
    for (u32 i = 0; i < META_FILES; ++i) swprintf(paths[i], MAX_PATH, L"%s\\ezb_%04u.tmp", dir, i);
    u8 buf[META_BYTES];
    for (u32 op = 0; op < META_OPS; ++op) {
        for (u32 i = 0; i < META_FILES; ++i) {
            const u64 t0 = timer_ticks();
            bool ok = true;
            if (op == META_CREATE || op == META_OPEN) {
                const bool create = op == META_CREATE;
                HANDLE f = CreateFileW(paths[i], create ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD n = 0;
                ok = f != INVALID_HANDLE_VALUE &&
                     (create ? WriteFile(f, data, META_BYTES, &n, nullptr) : ReadFile(f, buf, META_BYTES, &n, nullptr)) &&
                     n == META_BYTES;
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            } else if (op == META_STAT) {
                WIN32_FILE_ATTRIBUTE_DATA fa;
                ok = GetFileAttributesExW(paths[i], GetFileExInfoStandard, &fa) != 0;
            } else {
                ok = DeleteFileW(paths[i]) != 0;
            }
            const u64 dt = timer_ticks() - t0;
            if (!ok) {
                snprintf(error, error_size, "%s failed (error %lu)", META_NAME[op], GetLastError());
                for (u32 k = 0; k < META_FILES; ++k) DeleteFileW(paths[k]);
                return false;
            }
            hist_add(&h[op], dt);
            busy[op] += dt * timer_tick_seconds();
        }
    }
    return true;
}

void disk_fs_meta(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static LatHist h[META_OPS];
    u8 data[META_BYTES];
    fill_pattern(data, META_BYTES, 0, 77);
    char error[96] = {};
    const f64 seconds = p->duration_ms / 1000.0;
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        wchar_t dir[MAX_PATH];
        if (!disk_work_path(t, L"ezb_meta", dir) || (!CreateDirectoryW(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) {
            res_fail(out, "%s cannot create a work folder (error %lu)", t->label, GetLastError());
            return;
        }
        live_phase(live, dname("%s create / open / stat / delete %u files", t->label, META_FILES), "ops/s");
        for (u32 op = 0; op < META_OPS; ++op) hist_reset(&h[op]);
        f64 busy[META_OPS] = {};
        const f64 t0 = timer_now();
        bool ok = true;
        do {
            ok = meta_round(dir, data, h, busy, error, sizeof(error));
            const f64 el = timer_now() - t0;
            u64 ops = 0;
            f64 b = 0.0;
            for (u32 op = 0; op < META_OPS; ++op) { ops += h[op].count; b += busy[op]; }
            live_sample(live, b > 0.0 ? ops / b : 0.0);
            live_progress(live, ((f32)i + (f32)(el / seconds < 1.0 ? el / seconds : 1.0)) / g_disk.count);
        } while (ok && timer_now() - t0 < seconds && !cancelled(cancel));
        disk_delete_tree(dir);
        if (!ok) { res_fail(out, "%s %s", t->label, error); return; }
        if (cancelled(cancel)) return;
        for (u32 op = 0; op < META_OPS; ++op) {
            res_add(out, dname("%s %s", t->label, META_NAME[op]), "ops/s", busy[op] > 0.0 ? h[op].count / busy[op] : 0.0);
            add_hist_metrics(out, t->label, META_NAME[op], &h[op], IOM_AVG | IOM_PCT);
        }
    }
}

// ---- copy workloads -----------------------------------------------------------------------------------------------
// Each set is written once without the OS cache (so the copy reads it from the drive), then copied with
// CopyFileExW the way Explorer does: large files (>= 16 MiB) unbuffered, small ones buffered. The timed part ends
// after every copy has been flushed to the drive (FlushFileBuffers per file); MB/s and files/s count the copied data.

enum CopySet { SET_ISO, SET_PROGRAM, SET_GAME, SET_COUNT };
static const char* const SET_NAME[SET_COUNT] = { "ISO", "Program", "Game" };
constexpr u32 COPY_MAX_FILES = 8192;
constexpr u32 COPY_BIG = 16 * 1024 * 1024;

// File sizes of a set (multiples of 4 KiB), same for every run with the same test file size.
static u32 make_set(CopySet set, u64 budget, u32* sizes) {
    if (set == SET_ISO) {
        sizes[0] = (u32)(budget > 0xFFF00000ull ? 0xFFF00000ull : budget);
        return 1;
    }
    u64 rng = 0x5EED0000ull + set, total = 0;
    u32 n = 0;
    while (total < budget && n < COPY_MAX_FILES) {
        const u32 r = (u32)(rng_next(&rng) % 100);
        u64 kib;
        if (set == SET_PROGRAM) kib = r < 60 ? 4 + rng_next(&rng) % 29 : r < 90 ? 64 + rng_next(&rng) % 449 : 1024 + rng_next(&rng) % 3073;
        else kib = r < 70 ? 256 + rng_next(&rng) % 1793 : 8192 + rng_next(&rng) % 24577;
        u64 bytes = kib / 4 * 4 * KiB;
        if (total + bytes > budget) bytes = (budget - total) / (4 * KiB) * (4 * KiB);
        if (!bytes) break;
        sizes[n++] = (u32)bytes;
        total += bytes;
    }
    return n;
}

static void set_path(wchar_t* out, const wchar_t* root, u32 i) {
    swprintf(out, MAX_PATH, L"%s\\ezb_d%03u\\ezb_%05u.bin", root, i / 100, i);
}

static bool write_sources(const wchar_t* root, const u32* sizes, u32 n, u8* buf, u64 buf_bytes, char* error, u32 error_size) {
    wchar_t path[MAX_PATH];
    for (u32 i = 0; i < n; ++i) {
        if (i % 100 == 0) {
            swprintf(path, MAX_PATH, L"%s\\ezb_d%03u", root, i / 100);
            CreateDirectoryW(path, nullptr);
        }
        set_path(path, root, i);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_NO_BUFFERING, nullptr);
        if (f == INVALID_HANDLE_VALUE) { snprintf(error, error_size, "cannot create a source file (error %lu)", GetLastError()); return false; }
        for (u64 left = sizes[i]; left;) {
            const DWORD chunk = (DWORD)(left < buf_bytes ? left : buf_bytes);
            DWORD done = 0;
            if (!WriteFile(f, buf, chunk, &done, nullptr) || done != chunk) {
                snprintf(error, error_size, "writing a source file failed (error %lu)", GetLastError());
                CloseHandle(f);
                return false;
            }
            left -= chunk;
        }
        CloseHandle(f);
    }
    return true;
}

static bool copy_set(const wchar_t* src, const wchar_t* dst, const u32* sizes, u32 n, BenchLive* live, const CancelFlag* cancel,
                     f32 from, f32 to, f64* seconds, char* error, u32 error_size) {
    wchar_t a[MAX_PATH], b[MAX_PATH];
    u64 total = 0, done = 0;
    for (u32 i = 0; i < n; ++i) total += sizes[i];
    const f64 t0 = timer_now();
    f64 last = t0;
    for (u32 i = 0; i < n && !cancelled(cancel); ++i) {
        if (i % 100 == 0) {
            swprintf(b, MAX_PATH, L"%s\\ezb_d%03u", dst, i / 100);
            CreateDirectoryW(b, nullptr);
        }
        set_path(a, src, i);
        set_path(b, dst, i);
        if (!CopyFileExW(a, b, nullptr, nullptr, nullptr, sizes[i] >= COPY_BIG ? COPY_FILE_NO_BUFFERING : 0)) {
            snprintf(error, error_size, "CopyFileEx failed (error %lu)", GetLastError());
            return false;
        }
        done += sizes[i];
        const f64 now = timer_now();
        if (now - last > 0.1) {
            live_sample(live, done / (now - t0) / 1e6);
            live_progress(live, from + (to - from) * (f32)((f64)done / total));
            last = now;
        }
    }
    for (u32 i = 0; i < n && !cancelled(cancel); ++i) {
        set_path(b, dst, i);
        HANDLE f = CreateFileW(b, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE || !FlushFileBuffers(f)) {
            snprintf(error, error_size, "flushing a copy failed (error %lu)", GetLastError());
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            return false;
        }
        CloseHandle(f);
    }
    *seconds = timer_now() - t0;
    return true;
}

void disk_files_copy(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel)) return;
    static u32 sizes[COPY_MAX_FILES];
    const u64 budgets[SET_COUNT] = { g_disk.file_bytes, g_disk.file_bytes / 2, g_disk.file_bytes };
    constexpr u64 BUF = 4 * MiB;
    u8* buf = (u8*)disk_alloc(BUF);
    if (!buf) { res_fail(out, "out of memory"); return; }
    fill_pattern(buf, BUF, 0, 99);
    char error[96] = {};
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        for (u32 s = 0; s < SET_COUNT && !cancelled(cancel); ++s) {
            wchar_t src[MAX_PATH], dst[MAX_PATH];
            disk_work_path(t, L"ezb_src", src);
            disk_work_path(t, L"ezb_dst", dst);
            ULARGE_INTEGER free_user = {};
            GetDiskFreeSpaceExW(t->work_dir, &free_user, nullptr, nullptr);
            if (free_user.QuadPart < 2 * budgets[s] + 256 * MiB) {
                res_fail(out, "%s not enough free space for the copy test", t->label);
                disk_free(buf);
                return;
            }
            const u32 n = make_set((CopySet)s, budgets[s], sizes);
            const f32 from = (f32)(i * SET_COUNT + s) / (g_disk.count * SET_COUNT), to = (f32)(i * SET_COUNT + s + 1) / (g_disk.count * SET_COUNT);
            live_phase(live, dname("%s %s set: writing %u source files", t->label, SET_NAME[s], n), "");
            CreateDirectoryW(src, nullptr);
            CreateDirectoryW(dst, nullptr);
            f64 secs = 0.0;
            bool ok = write_sources(src, sizes, n, buf, BUF, error, sizeof(error));
            if (ok && !cancelled(cancel)) {
                pause_ms(500, cancel);
                live_phase(live, dname("%s copying the %s set (%u files)", t->label, SET_NAME[s], n), "MB/s");
                ok = copy_set(src, dst, sizes, n, live, cancel, from, to, &secs, error, sizeof(error));
            }
            live_phase(live, dname("%s deleting the %s set", t->label, SET_NAME[s]), "");
            disk_delete_tree(src);
            disk_delete_tree(dst);
            if (!ok) { res_fail(out, "%s %s", t->label, error); disk_free(buf); return; }
            if (cancelled(cancel)) break;
            u64 bytes = 0;
            for (u32 k = 0; k < n; ++k) bytes += sizes[k];
            res_add(out, dname("%s %s copy", t->label, SET_NAME[s]), "MB/s", secs > 0.0 ? bytes / secs / 1e6 : 0.0);
            if (s != SET_ISO) res_add(out, dname("%s %s copy files/s", t->label, SET_NAME[s]), "files/s", secs > 0.0 ? n / secs : 0.0);
        }
    }
    disk_free(buf);
}
