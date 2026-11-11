// Raw device tests (HD Tune style): full-span read curve, surface scan and seek profile on the physical disk behind
// each tested drive. Read-only by construction: the device is opened with GENERIC_READ only, so no code path can
// write to it. Opening \\.\PhysicalDriveN for reading needs administrator rights.
#include "disk/disk_internal.h"
#include "core/elevation.h"
#include "core/timer.h"
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

static const char* const US = "\xC2\xB5s";

struct RawDisk {
    HANDLE h;               // synchronous, unbuffered, read-only
    DiskTarget t;           // copy of the target with path = \\.\PhysicalDriveN for io_run (also read-only)
    u64 size;
    u32 sector;
};

static bool raw_open(const DiskTarget* t, RawDisk* r, char* error, u32 error_size) {
    memset(r, 0, sizeof(*r));
    r->h = INVALID_HANDLE_VALUE;
    if (t->phys_disk < 0) { snprintf(error, error_size, "no physical disk found behind it"); return false; }
    r->t = *t;
    r->t.hold = nullptr;
    swprintf(r->t.path, MAX_PATH, L"\\\\.\\PhysicalDrive%d", t->phys_disk);
    r->h = CreateFileW(r->t.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING, nullptr);
    if (r->h == INVALID_HANDLE_VALUE) {
        snprintf(error, error_size, "cannot open PhysicalDrive%d for reading (error %lu)", t->phys_disk, GetLastError());
        return false;
    }
    DISK_GEOMETRY_EX geo = {};
    DWORD got = 0;
    if (!DeviceIoControl(r->h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &got, nullptr) ||
        geo.DiskSize.QuadPart <= 0) {
        snprintf(error, error_size, "cannot read the disk size (error %lu)", GetLastError());
        CloseHandle(r->h);
        r->h = INVALID_HANDLE_VALUE;
        return false;
    }
    r->size = (u64)geo.DiskSize.QuadPart;
    r->sector = geo.Geometry.BytesPerSector ? geo.Geometry.BytesPerSector : 512;
    r->t.logical_sector = r->t.sector = r->sector;
    return true;
}

static void raw_close(RawDisk* r) {
    if (r->h != INVALID_HANDLE_VALUE) CloseHandle(r->h);
    r->h = INVALID_HANDLE_VALUE;
}

// Elevation and per-target checks shared by the three tests. False = result already set (skip / fail).
static bool raw_precheck(BenchResult* out) {
    if (!is_elevated()) {
        res_skip(out, "needs administrator rights (raw read of the physical disk)");
        return false;
    }
    return true;
}

static bool read_at(HANDLE h, u8* buf, u32 bytes, u64 off, f64* us) {
    OVERLAPPED o = {};
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    const u64 t0 = timer_ticks();
    const BOOL ok = ReadFile(h, buf, bytes, &got, &o);
    *us = ticks_us(timer_ticks() - t0);
    return ok && got == bytes;
}

// ---- full-span read curve ---------------------------------------------------------------------------------------

constexpr u32 SPAN_ZONES = 100;
constexpr u64 SPAN_ZONE_READ = 64 * MiB;

void disk_raw_span(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel) || !raw_precheck(out)) return;
    static RawDisk rd;
    static IoResult r;
    char error[128] = {};
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Position", "GB");
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        if (!raw_open(t, &rd, error, sizeof(error))) { res_fail(out, "%s %s", t->label, error); return; }
        const f32 base = (f32)i / g_disk.count, span = 1.0f / g_disk.count;
        const u64 align = MiB;
        const u64 zone_read = rd.size / SPAN_ZONES < SPAN_ZONE_READ ? rd.size / SPAN_ZONES / align * align : SPAN_ZONE_READ;
        if (zone_read < align) { raw_close(&rd); res_fail(out, "%s disk too small", t->label); return; }
        static f64 mbs[SPAN_ZONES], pos[SPAN_ZONES];
        live_phase(live, dname("%s full-span read (PhysicalDrive%d)", t->label, t->phys_disk), "MB/s");
        bool ok = true;
        for (u32 z = 0; z < SPAN_ZONES && ok && !cancelled(cancel); ++z) {
            const u64 off = (rd.size - zone_read) / (SPAN_ZONES - 1) * z / align * align;
            const IoSpec s = { (u32)align, 4, 1, false, 100, false, false, off, zone_read, zone_read / align };
            IoRun run = {};
            run.groups = &s;
            run.group_count = 1;
            run.seconds = 60.0;
            run.live = LIVE_MBS;
            run.prog_from = run.prog_to = base + span * 0.8f * z / SPAN_ZONES;
            ok = io_run(&rd.t, &run, &r, live, cancel, error, sizeof(error));
            mbs[z] = io_mbps(&r);
            pos[z] = off / 1e9;
            live_sample(live, mbs[z]);
        }
        // Access time: QD1 4K reads at random positions over the whole disk; burst: 64 KiB reads within 1 MiB.
        static IoResult acc, burst;
        if (ok && !cancelled(cancel)) {
            live_phase(live, dname("%s random access time", t->label), US);
            const IoSpec s = { 4096, 1, 1, true, 100, false, false, 0, rd.size / 4096 * 4096 };
            ok = io_run1(&rd.t, &s, 2.0, LIVE_LAT_US, base + span * 0.8f, base + span * 0.95f, &acc, live, cancel, error, sizeof(error));
        }
        if (ok && !cancelled(cancel)) {
            live_phase(live, dname("%s burst rate", t->label), "MB/s");
            const IoSpec s = { 64 * 1024, 4, 1, true, 100, false, false, 0, MiB };
            ok = io_run1(&rd.t, &s, 1.0, LIVE_MBS, base + span * 0.95f, base + span, &burst, live, cancel, error, sizeof(error));
        }
        raw_close(&rd);
        if (!ok) { res_fail(out, "%s %s", t->label, error); return; }
        if (cancelled(cancel)) return;
        f64 lo = mbs[0], hi = mbs[0], sum = 0.0;
        for (u32 z = 0; z < SPAN_ZONES; ++z) {
            res_add(out, dname("%s Read", t->label), "MB/s", mbs[z], pos[z]);
            lo = mbs[z] < lo ? mbs[z] : lo;
            hi = mbs[z] > hi ? mbs[z] : hi;
            sum += mbs[z];
        }
        const f64 end = rd.size / 1e9;
        res_add(out, dname("%s Minimum", t->label), "MB/s", lo, end);
        res_add(out, dname("%s Average", t->label), "MB/s", sum / SPAN_ZONES, end);
        res_add(out, dname("%s Maximum", t->label), "MB/s", hi, end);
        res_add(out, dname("%s Access time", t->label), US, hist_avg_us(&acc.hist[0]), end);
        res_add(out, dname("%s Burst rate", t->label), "MB/s", io_mbps(&burst), end);
    }
}

// ---- surface scan --------------------------------------------------------------------------------------------------
// Reads every sector in 4 MiB blocks (QD1, synchronous), classifies each block by its read time like HD Tune's
// error scan and keeps going past read errors.

constexpr u32 SCAN_BLOCK = 4 * 1024 * 1024;
static const f64 SCAN_CLASS_MS[] = { 10.0, 50.0, 150.0, 500.0 };
static const char* const SCAN_CLASS_NAME[] = { "Blocks < 10 ms", "Blocks 10-50 ms", "Blocks 50-150 ms", "Blocks 150-500 ms",
                                               "Blocks > 500 ms" };

void disk_raw_scan(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel) || !raw_precheck(out)) return;
    static RawDisk rd;
    char error[128] = {};
    u8* buf = (u8*)disk_alloc(SCAN_BLOCK);
    if (!buf) { res_fail(out, "out of memory"); return; }
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        if (!raw_open(t, &rd, error, sizeof(error))) { res_fail(out, "%s %s", t->label, error); break; }
        live_phase(live, dname("%s surface scan (PhysicalDrive%d, %.0f GB)", t->label, t->phys_disk, rd.size / 1e9), "MB/s");
        u64 classes[ARRAY_COUNT(SCAN_CLASS_NAME)] = {};
        u64 errors = 0, first_error = 0, done = 0;
        f64 slowest = 0.0;
        const f64 t0 = timer_now();
        f64 last = t0;
        for (u64 off = 0; off < rd.size && !cancelled(cancel); off += SCAN_BLOCK) {
            const u32 n = (u32)(rd.size - off < SCAN_BLOCK ? (rd.size - off) / rd.sector * rd.sector : SCAN_BLOCK);
            if (!n) break;
            f64 us;
            if (!read_at(rd.h, buf, n, off, &us)) {
                if (!errors++) first_error = off;
            } else {
                const f64 ms = us / 1000.0;
                u32 c = 0;
                while (c < ARRAY_COUNT(SCAN_CLASS_MS) && ms >= SCAN_CLASS_MS[c]) ++c;
                ++classes[c];
                slowest = ms > slowest ? ms : slowest;
            }
            done += n;
            const f64 now = timer_now();
            if (now - last > 0.1) {
                live_sample(live, done / (now - t0) / 1e6);
                live_progress(live, ((f32)i + (f32)((f64)done / rd.size)) / g_disk.count);
                last = now;
            }
        }
        const f64 secs = timer_now() - t0;
        raw_close(&rd);
        if (cancelled(cancel)) break;
        res_add(out, dname("%s Read speed", t->label), "MB/s", secs > 0.0 ? done / secs / 1e6 : 0.0);
        res_add(out, dname("%s Scanned", t->label), "GB", done / 1e9);
        for (u32 c = 0; c < ARRAY_COUNT(SCAN_CLASS_NAME); ++c) res_add(out, dname("%s %s", t->label, SCAN_CLASS_NAME[c]), "blocks", (f64)classes[c]);
        res_add(out, dname("%s Read errors (4 MiB blocks)", t->label), "blocks", (f64)errors);
        res_add(out, dname("%s Slowest block", t->label), "ms", slowest);
        if (errors) res_add(out, dname("%s First error at", t->label), "GB", first_error / 1e9);
    }
    disk_free(buf);
}

// ---- seek profile ----------------------------------------------------------------------------------------------------
// Access time vs seek distance: read 4K at a random position, then 4K at a distance of 1/2^k of the disk from it
// (alternating direction) and time the second read. Hard disks show their seek curve; SSDs are flat.

constexpr u32 SEEK_STEPS = 16;      // distances: whole disk / 2^0 .. 2^15
constexpr u32 SEEK_SAMPLES = 40;

void disk_raw_seek(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!disk_begin_test(p, live, out, cancel) || !raw_precheck(out)) return;
    static RawDisk rd;
    char error[128] = {};
    u8* buf = (u8*)disk_alloc(4096);
    if (!buf) { res_fail(out, "out of memory"); return; }
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Seek distance", "% of disk");
    u64 rng = timer_ticks() | 1;
    for (u32 i = 0; i < g_disk.count && !cancelled(cancel); ++i) {
        const DiskTarget* t = &g_disk.targets[i];
        if (!raw_open(t, &rd, error, sizeof(error))) { res_fail(out, "%s %s", t->label, error); break; }
        live_phase(live, dname("%s seek profile (PhysicalDrive%d)", t->label, t->phys_disk), "ms");
        const u64 span = rd.size / 4096 * 4096 - 4096;
        static f64 ms[SEEK_STEPS];
        bool ok = true;
        for (u32 k = 0; k < SEEK_STEPS && ok && !cancelled(cancel); ++k) {
            const u64 dist = span >> k & ~4095ull;
            f64 sum = 0.0;
            for (u32 j = 0; j < SEEK_SAMPLES && ok; ++j) {
                const u64 room = span - dist;
                const u64 a = room ? rng_next(&rng) % (room / 4096 + 1) * 4096 : 0;
                const u64 from = j & 1 ? a + dist : a, to = j & 1 ? a : a + dist;
                f64 us;
                ok = read_at(rd.h, buf, 4096, from, &us) && read_at(rd.h, buf, 4096, to, &us);
                sum += us;
            }
            ms[k] = sum / SEEK_SAMPLES / 1000.0;
            live_sample(live, ms[k]);
            live_progress(live, ((f32)i + (f32)(k + 1) / SEEK_STEPS) / g_disk.count);
        }
        if (!ok) snprintf(error, sizeof(error), "ReadFile failed (error %lu)", GetLastError());
        raw_close(&rd);
        if (!ok) { res_fail(out, "%s %s", t->label, error); break; }
        if (cancelled(cancel)) break;
        for (u32 k = SEEK_STEPS; k-- > 0;) res_add(out, dname("%s Access time", t->label), "ms", ms[k], 100.0 / (1u << k));
    }
    disk_free(buf);
}
