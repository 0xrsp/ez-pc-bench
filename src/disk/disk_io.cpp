// Latency histograms and the overlapped I/O engine shared by all file-based disk tests (see disk_internal.h).
#include "disk/disk_internal.h"
#include "core/perf.h"
#include "core/timer.h"
#include <stdio.h>
#include <string.h>

// ---- histogram -----------------------------------------------------------------------------------------

void hist_reset(LatHist* h) { memset(h, 0, sizeof(*h)); }

void hist_merge(LatHist* dst, const LatHist* src) {
    dst->count += src->count;
    dst->sum_ticks += src->sum_ticks;
    if (src->max_ticks > dst->max_ticks) dst->max_ticks = src->max_ticks;
    for (u32 b = 0; b < HIST_BUCKETS; ++b) dst->buckets[b] += src->buckets[b];
}

f64 ticks_us(u64 ticks) { return (f64)ticks * timer_tick_seconds() * 1e6; }

static void bucket_range(u32 b, u64* lo, u64* width) {
    if (b < 2 * HIST_SUB) { *lo = b; *width = 1; return; }
    const u32 e = HIST_SUB_BITS + 1 + (b - 2 * HIST_SUB) / HIST_SUB;
    const u64 sub = (b - 2 * HIST_SUB) % HIST_SUB;
    *lo = (HIST_SUB + sub) << (e - HIST_SUB_BITS);
    *width = 1ull << (e - HIST_SUB_BITS);
}

f64 hist_bucket_upper_us(u32 b) {
    u64 lo, w;
    bucket_range(b, &lo, &w);
    return ticks_us(lo + w);
}

f64 hist_percentile_us(const LatHist* h, f64 pct) {
    if (!h->count) return 0.0;
    if (pct >= 100.0) return ticks_us(h->max_ticks);
    u64 target = (u64)(pct / 100.0 * (f64)h->count + 0.999999);
    if (target < 1) target = 1;
    u64 seen = 0;
    for (u32 b = 0; b < HIST_BUCKETS; ++b) {
        seen += h->buckets[b];
        if (seen < target) continue;
        u64 lo, w;
        bucket_range(b, &lo, &w);
        const f64 mid = (f64)lo + (w > 1 ? (f64)w * 0.5 : 0.0);
        return ticks_us(mid < (f64)h->max_ticks ? (u64)mid : h->max_ticks);
    }
    return ticks_us(h->max_ticks);
}

f64 hist_avg_us(const LatHist* h) { return h->count ? ticks_us(h->sum_ticks) / (f64)h->count : 0.0; }

// ---- metrics -------------------------------------------------------------------------------------------

static const char* const PCT_NAME[] = { "p50", "p90", "p99", "p99.9", "p99.99", "max" };
static const f64 PCT_VALUE[] = { 50.0, 90.0, 99.0, 99.9, 99.99, 100.0 };
static const char* const US = "\xC2\xB5s";

void add_hist_metrics(BenchResult* out, const char* label, const char* phase, const LatHist* h, u32 flags) {
    if (flags & IOM_AVG) res_add(out, dname("%s %s latency", label, phase), US, hist_avg_us(h));
    if (flags & IOM_PCT)
        for (u32 i = 0; i < ARRAY_COUNT(PCT_NAME); ++i)
            res_add(out, dname("%s %s %s", label, phase, PCT_NAME[i]), US, hist_percentile_us(h, PCT_VALUE[i]));
}

void add_io_metrics(BenchResult* out, const char* label, const char* phase, const IoResult* r, u32 dir, u32 flags) {
    if (flags & IOM_THROUGHPUT) {
        res_add(out, dname("%s %s", label, phase), "MB/s", io_mbps(r));
        res_add(out, dname("%s %s IOPS", label, phase), "IOPS", io_iops(r));
    }
    LatHist h;
    const LatHist* src = &r->hist[dir == 1 ? 1 : 0];
    if (dir == 2) {
        hist_reset(&h);
        hist_merge(&h, &r->hist[0]);
        hist_merge(&h, &r->hist[1]);
        src = &h;
    }
    add_hist_metrics(out, label, phase, src, flags);
}

// ---- engine ----------------------------------------------------------------------------------------------

struct IoPass;

struct alignas(64) IoWorker {
    IoPass* pass;
    const IoSpec* spec;
    HANDLE file, iocp, thread;
    u8* buf;                        // qd * block
    u64 first_block, blocks;        // region in blocks
    u64 rng, seq_next;
    u64 issue_tick[IO_MAX_QD];
    u8 slot_write[IO_MAX_QD];
    OVERLAPPED ov[IO_MAX_QD];
    std::atomic<u64> live_ios, live_lat;    // read by the controller for the live display
    u64 ios[2];
    u64 issued, end_tick;
    LatHist hist[2];
    DWORD error;
    bool error_write;
};

struct IoPass {
    HANDLE start;                   // manual-reset event
    std::atomic<bool> stop;
    u32 workers;
    IoWorker w[IO_MAX_WORKERS];
};
static IoPass s_pass;

static bool submit(IoWorker* w, u32 slot) {
    const IoSpec* s = w->spec;
    if (s->max_ios && w->issued >= s->max_ios) return false;
    ++w->issued;
    u64 block_index;
    if (s->random) {
        block_index = w->first_block + rng_next(&w->rng) % w->blocks;
    } else {
        block_index = w->first_block + w->seq_next;
        if (++w->seq_next >= w->blocks) w->seq_next = 0;
    }
    const bool write = s->read_pct == 0 || (s->read_pct < 100 && rng_next(&w->rng) % 100 >= s->read_pct);
    const u64 off = block_index * s->block;
    OVERLAPPED* o = &w->ov[slot];
    memset(o, 0, sizeof(*o));
    o->Offset = (DWORD)off;
    o->OffsetHigh = (DWORD)(off >> 32);
    u8* b = w->buf + (u64)slot * s->block;
    w->slot_write[slot] = write;
    w->issue_tick[slot] = timer_ticks();
    const BOOL ok = write ? WriteFile(w->file, b, s->block, nullptr, o) : ReadFile(w->file, b, s->block, nullptr, o);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        w->error = GetLastError();
        w->error_write = write;
        return false;
    }
    return true;
}

static DWORD WINAPI io_thread(void* arg) {
    IoWorker* w = (IoWorker*)arg;
    IoPass* pass = w->pass;
    const IoSpec* s = w->spec;
    perf_setup_worker_thread();
    WaitForSingleObject(pass->start, INFINITE);

    u32 outstanding = 0;
    for (u32 k = 0; k < s->qd && !w->error && !pass->stop.load(std::memory_order_relaxed); ++k) outstanding += submit(w, k);

    OVERLAPPED_ENTRY entries[64];
    u64 live_ios = 0, live_lat = 0;
    bool stopping = w->error != 0;
    bool cancelled_io = false;
    u64 stop_tick = 0;
    const u64 grace_ticks = (u64)(1.0 / timer_tick_seconds());
    while (outstanding) {
        ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(w->iocp, entries, ARRAY_COUNT(entries), &n, 50, FALSE)) {
            if (GetLastError() != WAIT_TIMEOUT && !w->error) w->error = GetLastError();
            n = 0;
        }
        u64 now = timer_ticks();
        stopping = stopping || w->error || pass->stop.load(std::memory_order_relaxed);
        if (stopping && !stop_tick) stop_tick = now;
        for (ULONG i = 0; i < n; ++i) {
            const u32 slot = (u32)(entries[i].lpOverlapped - w->ov);
            const bool write = w->slot_write[slot];
            --outstanding;
            if (entries[i].lpOverlapped->Internal != 0 || entries[i].dwNumberOfBytesTransferred != s->block) {
                DWORD got = 0;      // GetOverlappedResult turns the NTSTATUS into a Win32 error
                const bool short_io = GetOverlappedResult(w->file, entries[i].lpOverlapped, &got, FALSE);
                if (!cancelled_io && !w->error) {
                    w->error = short_io ? ERROR_HANDLE_EOF : GetLastError();
                    w->error_write = write;
                }
                stopping = true;
                continue;
            }
            if (stopping) {
                // Drained after the stop: not counted for throughput (the pass is over), but its latency is real,
                // so a request that stalled across the end of the pass still shows up in the tail.
                hist_add(&w->hist[write], now - w->issue_tick[slot]);
                continue;
            }
            if (write && s->flush) {
                if (!FlushFileBuffers(w->file)) {
                    w->error = GetLastError();
                    w->error_write = true;
                    stopping = true;
                    continue;
                }
                now = timer_ticks();
            }
            const u64 lat = now - w->issue_tick[slot];
            hist_add(&w->hist[write], lat);
            ++w->ios[write];
            ++live_ios;
            live_lat += lat;
            outstanding += submit(w, slot);
        }
        w->live_ios.store(live_ios, std::memory_order_relaxed);
        w->live_lat.store(live_lat, std::memory_order_relaxed);
        if (stopping && outstanding && !cancelled_io && (w->error || now - stop_tick > grace_ticks)) {
            // Requests in flight normally finish within milliseconds; cancel after 1 s so a stuck device can't hang us.
            CancelIoEx(w->file, nullptr);
            cancelled_io = true;
        }
    }
    w->end_tick = timer_ticks();
    return 0;
}

static void close_workers(IoPass* pass) {
    for (u32 i = 0; i < pass->workers; ++i) {
        IoWorker* w = &pass->w[i];
        if (w->iocp) CloseHandle(w->iocp);
        if (w->file && w->file != INVALID_HANDLE_VALUE) CloseHandle(w->file);
        w->iocp = w->file = nullptr;
    }
    if (pass->start) CloseHandle(pass->start);
    pass->start = nullptr;
    pass->workers = 0;
}

static bool open_worker(IoWorker* w, const DiskTarget* t, const IoSpec* s, char* error, u32 error_size) {
    DWORD flags = FILE_FLAG_OVERLAPPED | (g_disk.buffered ? 0 : FILE_FLAG_NO_BUFFERING);
    if (s->write_through || g_disk.write_through) flags |= FILE_FLAG_WRITE_THROUGH;
    const DWORD access = s->read_pct == 100 ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    w->file = CreateFileW(t->path, access, SHARE_ALL, nullptr, OPEN_EXISTING, flags, nullptr);
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
    return true;
}

bool io_run(const DiskTarget* t, const IoRun* run, IoResult* out, BenchLive* live, const CancelFlag* cancel,
            char* error, u32 error_size) {
    IoPass* pass = &s_pass;
    u32 total = 0;
    u64 buf_bytes = 0;
    for (u32 g = 0; g < run->group_count; ++g) {
        const IoSpec* s = &run->groups[g];
        const u64 region = s->region_bytes ? s->region_bytes : g_disk.file_bytes - s->region_off;
        if (!s->block || s->block % t->logical_sector) {
            snprintf(error, error_size, "block size %u B is not a multiple of the %u B sector", s->block, t->logical_sector);
            return false;
        }
        if (s->qd < 1 || s->qd > IO_MAX_QD || s->threads < 1 || region / s->block < (u64)s->qd * s->threads) {
            snprintf(error, error_size, "test file too small for this test");
            return false;
        }
        total += s->threads;
        buf_bytes += (u64)s->threads * s->qd * s->block;
    }
    if (total > IO_MAX_WORKERS) { snprintf(error, error_size, "too many threads (max %u)", IO_MAX_WORKERS); return false; }
    u8* buf = (u8*)disk_alloc(buf_bytes);
    if (!buf) { snprintf(error, error_size, "out of memory (%llu MiB of buffers)", buf_bytes / MiB); return false; }
    fill_pattern(buf, buf_bytes, run->zero_pct, timer_ticks());

    for (u32 i = 0; i < total; ++i) {
        IoWorker* w = &pass->w[i];
        w->file = w->iocp = w->thread = nullptr;
        w->live_ios.store(0, std::memory_order_relaxed);
        w->live_lat.store(0, std::memory_order_relaxed);
        w->ios[0] = w->ios[1] = 0;
        w->issued = w->end_tick = 0;
        hist_reset(&w->hist[0]);
        hist_reset(&w->hist[1]);
        w->error = 0;
        w->error_write = false;
    }
    pass->workers = total;
    pass->start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pass->stop.store(false);
    bool ok = pass->start != nullptr;
    u64 seed = timer_ticks();
    u8* p = buf;
    for (u32 g = 0, i = 0; g < run->group_count && ok; ++g) {
        const IoSpec* s = &run->groups[g];
        const u64 region = s->region_bytes ? s->region_bytes : g_disk.file_bytes - s->region_off;
        for (u32 k = 0; k < s->threads && ok; ++k, ++i) {
            IoWorker* w = &pass->w[i];
            w->pass = pass;
            w->spec = s;
            w->buf = p;
            p += (u64)s->qd * s->block;
            w->first_block = s->region_off / s->block;
            w->blocks = region / s->block;
            w->rng = (seed += 0x9E3779B97F4A7C15ull) | 1;
            w->seq_next = w->blocks * k / s->threads;
            ok = open_worker(w, t, s, error, error_size);
        }
    }
    for (u32 i = 0; i < total && ok; ++i) {
        IoWorker* w = &pass->w[i];
        w->thread = CreateThread(nullptr, 0, io_thread, w, 0, nullptr);
        if (!w->thread) {
            snprintf(error, error_size, "CreateThread failed (error %lu)", GetLastError());
            ok = false;
        }
    }
    if (!ok) {
        pass->stop.store(true);
        if (pass->start) SetEvent(pass->start);
        for (u32 i = 0; i < total; ++i)
            if (pass->w[i].thread) { WaitForSingleObject(pass->w[i].thread, INFINITE); CloseHandle(pass->w[i].thread); }
        close_workers(pass);
        disk_free(buf);
        return false;
    }

    // Workers of the live group: [lw0, lw1).
    u32 lw0 = 0;
    for (u32 g = 0; g < run->live_group; ++g) lw0 += run->groups[g].threads;
    const u32 lw1 = lw0 + run->groups[run->live_group].threads;
    const u32 live_block = run->groups[run->live_group].block;

    HANDLE threads[IO_MAX_WORKERS];
    for (u32 i = 0; i < total; ++i) threads[i] = pass->w[i].thread;
    const u64 start_tick = timer_ticks();
    const f64 t0 = timer_now();
    SetEvent(pass->start);
    f64 now = t0, last_t = t0;
    u64 last_ios = 0, last_lat = 0;
    bool all_done = false;
    while (now - t0 < run->seconds && !cancelled(cancel) && !all_done) {
        all_done = WaitForMultipleObjects(total, threads, TRUE, 100) == WAIT_OBJECT_0;     // bounded runs end early
        now = timer_now();
        u64 ios = 0, lat = 0;
        bool failed = false;
        for (u32 i = 0; i < total; ++i) {
            if (i >= lw0 && i < lw1) {
                ios += pass->w[i].live_ios.load(std::memory_order_relaxed);
                lat += pass->w[i].live_lat.load(std::memory_order_relaxed);
            }
            failed |= pass->w[i].error != 0;
        }
        if (now > last_t && live) {
            const f64 dios = (f64)(ios - last_ios);
            if (run->live == LIVE_LAT_US) { if (dios > 0) live_sample(live, ticks_us(lat - last_lat) / dios); }
            else if (run->live == LIVE_IOPS) live_sample(live, dios / (now - last_t));
            else live_sample(live, dios * live_block / (now - last_t) / 1e6);
        }
        last_ios = ios;
        last_lat = lat;
        last_t = now;
        const f64 frac = (now - t0) / run->seconds;
        if (live) live_progress(live, run->prog_from + (run->prog_to - run->prog_from) * (f32)(frac < 1.0 ? frac : 1.0));
        if (failed) break;
    }
    pass->stop.store(true);
    const f64 t1 = timer_now();

    DWORD err = 0;
    bool err_write = false;
    for (u32 g = 0, i = 0; g < run->group_count; ++g) {
        IoResult* r = &out[g];
        memset(r, 0, sizeof(*r));
        u64 end_tick = 0;
        for (u32 k = 0; k < run->groups[g].threads; ++k, ++i) {
            IoWorker* w = &pass->w[i];
            WaitForSingleObject(w->thread, INFINITE);
            CloseHandle(w->thread);
            w->thread = nullptr;
            for (u32 d = 0; d < 2; ++d) {
                r->ios[d] += w->ios[d];
                hist_merge(&r->hist[d], &w->hist[d]);
            }
            if (w->end_tick > end_tick) end_tick = w->end_tick;
            if (w->error && !err) { err = w->error; err_write = w->error_write; }
        }
        r->bytes = (r->ios[0] + r->ios[1]) * run->groups[g].block;
        // A bounded group (max_ios) is timed from start to its last completion, a timed one to the stop signal.
        r->seconds = run->groups[g].max_ios ? (end_tick - start_tick) * timer_tick_seconds() : t1 - t0;
    }
    close_workers(pass);
    disk_free(buf);
    if (err && !cancelled(cancel)) {
        snprintf(error, error_size, "%s failed (error %lu)", err_write ? "WriteFile" : "ReadFile", err);
        return false;
    }
    return true;
}

bool io_run1(const DiskTarget* t, const IoSpec* spec, f64 seconds, LiveKind lk, f32 from, f32 to, IoResult* out,
             BenchLive* live, const CancelFlag* cancel, char* error, u32 error_size) {
    IoRun run = {};
    run.groups = spec;
    run.group_count = 1;
    run.seconds = seconds;
    run.live = lk;
    run.zero_pct = disk_zero_pct();
    run.prog_from = from;
    run.prog_to = to;
    return io_run(t, &run, out, live, cancel, error, error_size);
}
