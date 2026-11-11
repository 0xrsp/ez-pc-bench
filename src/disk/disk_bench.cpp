// Disk suite (CrystalDiskMark-style): sequential / random reads and writes on one test file with
// unbuffered overlapped I/O. Queue depth = outstanding requests per thread, completed through a
// per-thread I/O completion port and resubmitted immediately. Best of N passes is reported.
#include "core/bench.h"
#include "core/elevation.h"
#include "core/perf.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef std::atomic<bool> CancelFlag;

constexpr u32 MAX_QD = 32;
constexpr u32 MAX_THREADS = 16;
constexpr u32 MAX_PASSES = 9;
constexpr u64 FILL_CHUNK = 1 * MiB;
constexpr u64 FREE_MARGIN = 64 * MiB;    // keep this much free on the volume after creating the file
constexpr u32 SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// One test file per selected drive. Metric and phase names live in letter-indexed static tables because
// results keep pointing at them after the suite has shut down.
struct DiskTarget {
    char letter;
    wchar_t path[MAX_PATH];
    wchar_t dir[MAX_PATH];      // folder to remove at shutdown ("" = none)
    HANDLE hold;                // fill handle, kept open; DELETE_ON_CLOSE also cleans up after a crash
    u32 sector;
};

struct DiskSuite {
    DiskTarget targets[26];
    u32 count;
    u64 file_bytes;
    bool write_through;
};
static DiskSuite s_disk;

enum DiskMetric : u8 { DM_READ, DM_READ_IOPS, DM_READ_LAT, DM_WRITE, DM_WRITE_IOPS, DM_WRITE_LAT, DM_COUNT };
static const char* const METRIC_SUFFIX[DM_COUNT] = { "Read", "Read IOPS", "Read latency", "Write", "Write IOPS", "Write latency" };
static char s_metric_names[26][DM_COUNT][24];   // "C: Read", ...
static char s_prepare_phase[26][40];            // "C: preparing test file"

static u64 rng_next(u64* s) {     // xorshift64*
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static void fill_random(u8* buf, u64 bytes, u64 seed) {
    u64 s = seed | 1;
    u64* p = (u64*)buf;
    for (u64 i = 0; i < bytes / 8; ++i) p[i] = rng_next(&s);
}

// ---- suite: test files -------------------------------------------------------------------
// Location per drive: <drive>:\ezbench.tmp when elevated (like CrystalDiskMark), else <drive>:\ezbench\ezbench.tmp
// (users may create folders but not files in a drive root), else %TEMP% when it is on the same drive.

// Creates the folder if needed. Sets *created when this call made it.
static bool ensure_dir(const wchar_t* dir, bool* created) {
    *created = false;
    if (CreateDirectoryW(dir, nullptr)) { *created = true; return true; }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Tries one candidate location: deletes a stale file, checks free space, creates the file.
static bool try_create(DiskTarget* t, const wchar_t* dir, const wchar_t* path, char* error, u32 error_size) {
    DeleteFileW(path);      // stale file from a crashed run
    ULARGE_INTEGER free_user;
    if (GetDiskFreeSpaceExW(dir, &free_user, nullptr, nullptr) && free_user.QuadPart < s_disk.file_bytes + FREE_MARGIN) {
        snprintf(error, error_size, "not enough free space (%llu MiB needed, %llu MiB free)",
                 (s_disk.file_bytes + FREE_MARGIN) / MiB, free_user.QuadPart / MiB);
        return false;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, SHARE_ALL, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(error, error_size, "cannot create test file (error %lu)", GetLastError());
        return false;
    }
    t->hold = h;
    wcscpy_s(t->path, path);
    return true;
}

// Writes the whole file sequentially with incompressible data (unbuffered, synchronous).
static bool fill_file(DiskTarget* t, char* error, u32 error_size) {
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG)s_disk.file_bytes;
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(t->hold, size, nullptr, FILE_BEGIN) || !SetEndOfFile(t->hold) ||
        !SetFilePointerEx(t->hold, zero, nullptr, FILE_BEGIN)) {
        snprintf(error, error_size, "cannot size test file (error %lu)", GetLastError());
        return false;
    }
    u8* buf = (u8*)VirtualAlloc(nullptr, FILL_CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { snprintf(error, error_size, "out of memory"); return false; }

    live_phase(&g_live, s_prepare_phase[t->letter - 'A'], "MB/s");
    const f64 t0 = timer_now();
    f64 last = t0;
    bool ok = true;
    for (u64 off = 0; off < s_disk.file_bytes; off += FILL_CHUNK) {
        fill_random(buf, FILL_CHUNK, off * 0x9E3779B97F4A7C15ull + 12345);
        DWORD written = 0;
        if (!WriteFile(t->hold, buf, (DWORD)FILL_CHUNK, &written, nullptr) || written != FILL_CHUNK) {
            snprintf(error, error_size, "writing test file failed (error %lu)", GetLastError());
            ok = false;
            break;
        }
        const f64 now = timer_now();
        if (now - last > 0.1) {
            live_sample(&g_live, (off + FILL_CHUNK) / (now - t0) / 1e6);
            live_progress(&g_live, (f32)((f64)(off + FILL_CHUNK) / (f64)s_disk.file_bytes));
            last = now;
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    if (ok && !FlushFileBuffers(t->hold)) {
        snprintf(error, error_size, "flushing test file failed (error %lu)", GetLastError());
        ok = false;
    }
    return ok;
}

static void close_target(DiskTarget* t) {
    if (t->hold && t->hold != INVALID_HANDLE_VALUE) CloseHandle(t->hold);   // DELETE_ON_CLOSE removes the file
    if (t->path[0]) DeleteFileW(t->path);
    if (t->dir[0]) RemoveDirectoryW(t->dir);    // only succeeds when empty
    memset(t, 0, sizeof(*t));
}

void disk_shutdown(void*) {
    for (u32 i = 0; i < s_disk.count; ++i) close_target(&s_disk.targets[i]);
    memset(&s_disk, 0, sizeof(s_disk));
}

static bool open_target(DiskTarget* t, wchar_t drive, char* error, u32 error_size) {
    t->letter = (char)drive;
    DriveInfo info;
    if (!drive_query(drive, &info)) { snprintf(error, error_size, "drive not found"); return false; }
    t->sector = info.physical_sector > info.logical_sector ? info.physical_sector : info.logical_sector;
    if (t->sector == 0) t->sector = 4096;

    const wchar_t root[4] = { drive, L':', L'\\', 0 };
    wchar_t dir[MAX_PATH], sub_path[MAX_PATH], path[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%sezbench", root);
    swprintf(sub_path, MAX_PATH, L"%s\\ezbench.tmp", dir);
    // Leftovers of a crashed run: the file, then the folder (RemoveDirectoryW only succeeds when empty).
    DeleteFileW(sub_path);
    RemoveDirectoryW(dir);
    if (is_elevated()) {
        swprintf(path, MAX_PATH, L"%sezbench.tmp", root);
        if (try_create(t, root, path, error, error_size)) return fill_file(t, error, error_size);
    }
    bool created = false;
    if (ensure_dir(dir, &created)) {
        if (created) wcscpy_s(t->dir, dir);
        if (try_create(t, dir, sub_path, error, error_size)) return fill_file(t, error, error_size);
        if (created) RemoveDirectoryW(dir);
        t->dir[0] = 0;
    }
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n > 0 && n < MAX_PATH - 16 && towupper(temp[0]) == towupper(drive)) {
        swprintf(path, MAX_PATH, L"%sezbench.tmp", temp);
        if (try_create(t, temp, path, error, error_size)) return fill_file(t, error, error_size);
    }
    if (!error[0]) snprintf(error, error_size, "no writable location");
    return false;
}

bool disk_init(const BenchParams* p, void** state, char* error, u32 error_size) {
    memset(&s_disk, 0, sizeof(s_disk));
    *state = &s_disk;
    if ((p->disk_drives & ((1u << 26) - 1)) == 0) { snprintf(error, error_size, "no drive selected"); return false; }
    u64 bytes = p->disk_file_bytes / MiB * MiB;
    s_disk.file_bytes = bytes < 16 * MiB ? 16 * MiB : bytes;
    s_disk.write_through = p->disk_write_through;

    for (u32 d = 0; d < 26; ++d) {
        if (!((p->disk_drives >> d) & 1)) continue;
        const char letter = (char)('A' + d);
        for (u32 m = 0; m < DM_COUNT; ++m) snprintf(s_metric_names[d][m], sizeof(s_metric_names[d][m]), "%c: %s", letter, METRIC_SUFFIX[m]);
        snprintf(s_prepare_phase[d], sizeof(s_prepare_phase[d]), "%c: preparing test file", letter);
        char why[96] = {};
        DiskTarget* t = &s_disk.targets[s_disk.count++];
        if (!open_target(t, (wchar_t)(L'A' + d), why, sizeof(why))) {
            snprintf(error, error_size, "%c: %s", letter, why);
            disk_shutdown(nullptr);     // the runner only calls shutdown after a successful init
            return false;
        }
    }
    return true;
}

// ---- I/O engine ---------------------------------------------------------------------------

struct DiskTest {
    u32 block;
    u32 qd;
    u32 threads;
    bool random;
};

struct IoPass;

struct alignas(64) IoWorker {
    IoPass* pass;
    const DiskTest* test;
    u32 index;
    bool write;
    HANDLE file, iocp, thread;
    u8* buf;                        // qd * block
    u64 blocks;                     // file size in blocks
    u64 rng, seq_next;
    u64 issue_tick[MAX_QD];
    OVERLAPPED ov[MAX_QD];
    std::atomic<u64> ios;           // completions while measuring (read live by the controller)
    u64 lat_ticks;                  // sum of completion latencies
    DWORD error;
};

struct IoPass {
    HANDLE start;                   // manual-reset event
    std::atomic<bool> stop;
    u32 threads;
    IoWorker workers[MAX_THREADS];
};
static IoPass s_pass;

static bool submit(IoWorker* w, u32 slot) {
    const DiskTest* t = w->test;
    u64 block_index;
    if (t->random) {
        block_index = rng_next(&w->rng) % w->blocks;
    } else {
        block_index = w->seq_next;
        if (++w->seq_next >= w->blocks) w->seq_next = 0;
    }
    const u64 off = block_index * t->block;
    OVERLAPPED* o = &w->ov[slot];
    memset(o, 0, sizeof(*o));
    o->Offset = (DWORD)off;
    o->OffsetHigh = (DWORD)(off >> 32);
    u8* b = w->buf + (u64)slot * t->block;
    w->issue_tick[slot] = timer_ticks();
    const BOOL ok = w->write ? WriteFile(w->file, b, t->block, nullptr, o) : ReadFile(w->file, b, t->block, nullptr, o);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        w->error = GetLastError();
        return false;
    }
    return true;
}

static DWORD WINAPI io_thread(void* arg) {
    IoWorker* w = (IoWorker*)arg;
    IoPass* pass = w->pass;
    const u32 qd = w->test->qd;
    perf_setup_worker_thread();
    WaitForSingleObject(pass->start, INFINITE);

    u32 outstanding = 0;
    for (u32 s = 0; s < qd && !w->error; ++s) outstanding += submit(w, s);

    OVERLAPPED_ENTRY entries[MAX_QD];
    u64 ios = 0, lat = 0;
    bool stopping = w->error != 0;
    bool cancelled = false;
    while (outstanding) {
        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(w->iocp, entries, MAX_QD, &n, 50, FALSE)) {
            if (GetLastError() != WAIT_TIMEOUT && !w->error) w->error = GetLastError();
            n = 0;
        }
        const u64 now = timer_ticks();
        stopping = stopping || w->error || pass->stop.load(std::memory_order_relaxed);
        for (ULONG i = 0; i < n; ++i) {
            const u32 slot = (u32)(entries[i].lpOverlapped - w->ov);
            --outstanding;
            if (entries[i].lpOverlapped->Internal != 0 || entries[i].dwNumberOfBytesTransferred != w->test->block) {
                DWORD got = 0;      // GetOverlappedResult turns the NTSTATUS into a Win32 error
                const bool short_io = GetOverlappedResult(w->file, entries[i].lpOverlapped, &got, FALSE);
                if (!cancelled && !w->error) w->error = short_io ? ERROR_HANDLE_EOF : GetLastError();
                stopping = true;
                continue;
            }
            if (stopping) continue;
            ++ios;
            lat += now - w->issue_tick[slot];
            outstanding += submit(w, slot);
        }
        w->ios.store(ios, std::memory_order_relaxed);
        if (stopping && outstanding && !cancelled) {
            // Requests in flight finish in microseconds; cancel anyway so a stuck device can't hang us.
            CancelIoEx(w->file, nullptr);
            cancelled = true;
        }
    }
    w->lat_ticks = lat;
    return 0;
}

struct PassResult {
    f64 mbps, iops, lat_us;
};

// Runs one timed pass with all workers. Returns false on an I/O error (message in `error`).
static bool run_pass(const DiskTest* t, bool write, f64 seconds, BenchLive* live, const CancelFlag* cancel,
                     f32 prog_from, f32 prog_to, PassResult* out, char* error, u32 error_size) {
    IoPass* pass = &s_pass;
    ResetEvent(pass->start);
    pass->stop.store(false);
    const u64 seed = timer_ticks();
    for (u32 i = 0; i < pass->threads; ++i) {
        IoWorker* w = &pass->workers[i];
        w->write = write;
        w->rng = (seed + i * 0x9E3779B97F4A7C15ull) | 1;
        w->seq_next = w->blocks * i / pass->threads;
        w->ios.store(0);
        w->lat_ticks = 0;
        w->error = 0;
        w->thread = CreateThread(nullptr, 0, io_thread, w, 0, nullptr);
        if (!w->thread) {
            snprintf(error, error_size, "CreateThread failed (error %lu)", GetLastError());
            pass->stop.store(true);
            SetEvent(pass->start);
            for (u32 k = 0; k < i; ++k) { WaitForSingleObject(pass->workers[k].thread, INFINITE); CloseHandle(pass->workers[k].thread); }
            return false;
        }
    }

    const f64 t0 = timer_now();
    SetEvent(pass->start);
    f64 now = t0, last_t = t0;
    u64 last_ios = 0;
    while (now - t0 < seconds && !cancel->load(std::memory_order_relaxed)) {
        Sleep(100);
        now = timer_now();
        u64 ios = 0;
        bool failed = false;
        for (u32 i = 0; i < pass->threads; ++i) {
            ios += pass->workers[i].ios.load(std::memory_order_relaxed);
            failed |= pass->workers[i].error != 0;
        }
        if (now > last_t) live_sample(live, (f64)(ios - last_ios) * t->block / (now - last_t) / 1e6);
        last_ios = ios;
        last_t = now;
        const f64 frac = (now - t0) / seconds;
        live_progress(live, prog_from + (prog_to - prog_from) * (f32)(frac < 1.0 ? frac : 1.0));
        if (failed) break;
    }
    pass->stop.store(true);
    const f64 t1 = timer_now();

    u64 ios = 0, lat = 0;
    DWORD err = 0;
    for (u32 i = 0; i < pass->threads; ++i) {
        IoWorker* w = &pass->workers[i];
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
        w->thread = nullptr;
        ios += w->ios.load();
        lat += w->lat_ticks;
        if (w->error && !err) err = w->error;
    }
    if (err && !cancel->load(std::memory_order_relaxed)) {
        snprintf(error, error_size, "%s failed (error %lu)", write ? "WriteFile" : "ReadFile", err);
        return false;
    }
    const f64 secs = t1 - t0;
    out->iops = secs > 0.0 ? ios / secs : 0.0;
    out->mbps = out->iops * t->block / 1e6;
    out->lat_us = ios ? (f64)lat / ios * timer_tick_seconds() * 1e6 : 0.0;
    return true;
}

static void close_workers(IoPass* pass) {
    for (u32 i = 0; i < MAX_THREADS; ++i) {
        IoWorker* w = &pass->workers[i];
        if (w->iocp) CloseHandle(w->iocp);
        if (w->file && w->file != INVALID_HANDLE_VALUE) CloseHandle(w->file);
        w->iocp = w->file = nullptr;
    }
    if (pass->start) CloseHandle(pass->start);
    pass->start = nullptr;
}

// Opens one handle + completion port per thread; buffers come from `buf` (threads * qd * block).
static bool open_workers(const DiskTest* t, const DiskTarget* target, u8* buf, char* error, u32 error_size) {
    IoPass* pass = &s_pass;
    pass->threads = t->threads;
    pass->start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD flags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED;
    if (s_disk.write_through) flags |= FILE_FLAG_WRITE_THROUGH;
    for (u32 i = 0; i < t->threads; ++i) {
        IoWorker* w = &pass->workers[i];
        w->pass = pass;
        w->test = t;
        w->index = i;
        w->blocks = s_disk.file_bytes / t->block;
        w->buf = buf + (u64)i * t->qd * t->block;
        w->thread = nullptr;
        w->file = CreateFileW(target->path, GENERIC_READ | GENERIC_WRITE, SHARE_ALL, nullptr, OPEN_EXISTING, flags, nullptr);
        if (w->file == INVALID_HANDLE_VALUE) {
            w->file = nullptr;
            snprintf(error, error_size, "cannot open test file (error %lu)", GetLastError());
            return false;
        }
        w->iocp = CreateIoCompletionPort(w->file, nullptr, 0, 1);
        if (!w->iocp) {
            snprintf(error, error_size, "CreateIoCompletionPort failed (error %lu)", GetLastError());
            return false;
        }
    }
    return pass->start != nullptr;
}

// Sleeps up to `ms`, returning early on cancel.
static void pause_ms(u32 ms, const CancelFlag* cancel) {
    for (u32 t = 0; t < ms && !cancel->load(std::memory_order_relaxed); t += 20) Sleep(20);
}

static char s_phase_names[26][2][MAX_PASSES][32];   // static storage: BenchLive keeps the pointer

// Best read and write pass on one drive. False on an I/O error (message in `error`).
static bool run_target(const DiskTest* t, const DiskTarget* target, const BenchParams* p, BenchLive* live,
                       const CancelFlag* cancel, f32 prog_from, f32 prog_to, PassResult best[2], char* error, u32 error_size) {
    const u64 buf_bytes = (u64)t->threads * t->qd * t->block;    // at most 8 MiB (SEQ1M Q8)
    u8* buf = (u8*)VirtualAlloc(nullptr, buf_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { snprintf(error, error_size, "out of memory"); return false; }
    fill_random(buf, buf_bytes, timer_ticks());
    if (!open_workers(t, target, buf, error, error_size)) {
        close_workers(&s_pass);
        VirtualFree(buf, 0, MEM_RELEASE);
        return false;
    }

    const u32 passes = p->disk_passes < 1 ? 1 : p->disk_passes > MAX_PASSES ? MAX_PASSES : p->disk_passes;
    const f64 seconds = p->duration_ms / 1000.0;
    const u32 interval_ms = p->duration_ms / 4 < 1000 ? p->duration_ms / 4 : 1000;
    const f32 span = (prog_to - prog_from) / (2.0f * passes);
    bool ok = true;
    for (u32 dir = 0; dir < 2 && ok; ++dir) {
        for (u32 k = 0; k < passes && ok && !cancel->load(std::memory_order_relaxed); ++k) {
            char* name = s_phase_names[target->letter - 'A'][dir][k];
            snprintf(name, sizeof(s_phase_names[0][0][0]), "%c: %s pass %u/%u", target->letter, dir ? "Write" : "Read", k + 1, passes);
            live_phase(live, name, "MB/s");
            if (dir || k) pause_ms(interval_ms, cancel);
            const f32 from = prog_from + (dir * passes + k) * span;
            PassResult r = {};
            ok = run_pass(t, dir == 1, seconds, live, cancel, from, from + span, &r, error, error_size);
            if (ok && r.mbps > best[dir].mbps) best[dir] = r;
        }
    }
    close_workers(&s_pass);
    VirtualFree(buf, 0, MEM_RELEASE);
    return ok;
}

// Runs the test on every selected drive; metrics are named per drive ("C: Read", "D: Read", ...).
static void run_test(const DiskTest* t, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (s_disk.count == 0) { res_fail(out, "no test file"); return; }
    if (s_disk.file_bytes < (u64)t->block * t->qd * t->threads) { res_fail(out, "test file too small"); return; }
    for (u32 i = 0; i < s_disk.count; ++i) {
        const DiskTarget* target = &s_disk.targets[i];
        if (t->block % target->sector) {
            res_fail(out, "%c: block size not a multiple of the %u B sector", target->letter, target->sector);
            return;
        }
    }

    static PassResult best[26][2];
    memset(best, 0, sizeof(best));
    char error[96] = {};
    for (u32 i = 0; i < s_disk.count && !cancel->load(std::memory_order_relaxed); ++i) {
        const DiskTarget* target = &s_disk.targets[i];
        const f32 from = (f32)i / s_disk.count, to = (f32)(i + 1) / s_disk.count;
        if (!run_target(t, target, p, live, cancel, from, to, best[i], error, sizeof(error))) {
            res_fail(out, "%c: %s", target->letter, error);
            return;
        }
    }
    if (cancel->load(std::memory_order_relaxed)) return;

    res_begin(out, RESULT_VALUES);
    static const char* const UNITS[DM_COUNT] = { "MB/s", "IOPS", "\xC2\xB5s", "MB/s", "IOPS", "\xC2\xB5s" };
    for (u32 i = 0; i < s_disk.count; ++i) {
        const u32 d =(u32)(s_disk.targets[i].letter - 'A');
        const f64 v[DM_COUNT] = { best[i][0].mbps, best[i][0].iops, best[i][0].lat_us,
                                  best[i][1].mbps, best[i][1].iops, best[i][1].lat_us };
        for (u32 m = 0; m < DM_COUNT; ++m) res_add(out, s_metric_names[d][m], UNITS[m], v[m]);
    }
}

// ---- tests (CrystalDiskMark 8 "Default" and "NVMe" profiles) ----------------------------------

static const DiskTest T_SEQ1M_Q8T1    = { 1024 * 1024,  8, 1,  false };
static const DiskTest T_SEQ1M_Q1T1    = { 1024 * 1024,  1, 1,  false };
static const DiskTest T_RND4K_Q32T1   = { 4096,        32, 1,  true };
static const DiskTest T_RND4K_Q1T1    = { 4096,         1, 1,  true };
static const DiskTest T_SEQ128K_Q32T1 = { 128 * 1024,  32, 1,  false };
static const DiskTest T_RND4K_Q32T16  = { 4096,        32, 16, true };

void disk_seq1m_q8t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_test(&T_SEQ1M_Q8T1, p, live, out, cancel);
}
void disk_seq1m_q1t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_test(&T_SEQ1M_Q1T1, p, live, out, cancel);
}
void disk_rnd4k_q32t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_test(&T_RND4K_Q32T1, p, live, out, cancel);
}
void disk_rnd4k_q1t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_test(&T_RND4K_Q1T1, p, live, out, cancel);
}
void disk_seq128k_q32t1(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_test(&T_SEQ128K_Q32T1, p, live, out, cancel);
}
void disk_rnd4k_q32t16(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    run_test(&T_RND4K_Q32T16, p, live, out, cancel);
}
