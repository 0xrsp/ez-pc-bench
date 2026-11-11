// Memory error test (STAB_MEM): a large locked, pre-faulted buffer split into one region per logical processor;
// each thread runs pattern passes over its region and reads them back. Coverage is partial by nature: Windows,
// drivers and other programs keep the rest of RAM, and we only see virtual addresses (the physical address of
// an error would need a driver). MemTest86 boots bare-metal and reaches nearly all of it.
#include "stability/stab_internal.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/timer.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

constexpr u64 HEADROOM = 2 * GiB;           // always leave this much RAM available to Windows and other programs
constexpr u64 MIN_BYTES = 64 * MiB;
constexpr u64 BLOCK = 512;                  // words checked per OR-reduction (4 KiB)

enum MemPattern : u8 { MP_ADDRESS, MP_WALK1, MP_WALK0, MP_RANDOM, MP_INVERSIONS, MP_COUNT };
static const char* const PATTERN_NAME[MP_COUNT] = { "Address in address", "Walking 1s", "Walking 0s", "Random",
                                                    "Moving inversions" };

struct MemShared {
    u64* base;
    u64 region_words;           // per thread
    u32 threads;
    const u32* lps;
    f64 deadline, start;
    std::atomic<bool> injected;
};

struct Region {
    MemShared* s;
    u64* w;
    u64 n;
    i32 lp, chunk;
    f64 bytes_checked;
};

static void report(Region* r, u64 i, const char* what, u64 expected) {
    stab_error(STAB_MEM, r->lp, r->chunk, (u64)(uintptr_t)&r->w[i], what, expected, r->w[i]);
}

// Checks w[i] == f(i) for all i, block by block (an OR-reduction per block keeps it at memory speed); `then`
// (optional) rewrites each word after the check. Expected-value functions are tiny lambdas.
template <class Want, class Then>
static void check_pass(Region* r, const char* what, Want want, Then then, bool descending = false) {
    const u64 n = r->n;
    for (u64 b0 = 0; b0 < n; b0 += BLOCK) {
        const u64 lo = descending ? (n - (b0 + BLOCK < n ? b0 + BLOCK : n)) : b0;
        const u64 hi = descending ? n - b0 : (b0 + BLOCK < n ? b0 + BLOCK : n);
        u64 diff = 0;
        for (u64 i = lo; i < hi; ++i) diff |= r->w[i] ^ want(i);
        if (diff)
            for (u64 i = lo; i < hi; ++i)
                if (r->w[i] != want(i)) report(r, i, what, want(i));
        then(lo, hi);
        if ((b0 & 0xFFFF) == 0 && g_stab.stop.load(std::memory_order_relaxed)) return;
    }
    r->bytes_checked += (f64)n * 8;
}

static void no_rewrite(u64, u64) {}

static void maybe_inject(Region* r) {
    MemShared* s = r->s;
    if (g_stab.cfg.inject && r->chunk == 0 && timer_now() - s->start > 1.0 && !s->injected.exchange(true))
        r->w[r->n / 3] ^= 1ull << 17;   // simulated bit flip
}

static void run_pattern(Region* r, MemPattern p, u64 pass) {
    u64* w = r->w;
    const u64 n = r->n;
    const char* name = PATTERN_NAME[p];
    switch (p) {
        case MP_ADDRESS: {
            for (u64 i = 0; i < n; ++i) w[i] = (u64)(uintptr_t)&w[i];
            maybe_inject(r);
            check_pass(r, name, [w](u64 i) { return (u64)(uintptr_t)&w[i]; },
                       [w](u64 lo, u64 hi) { for (u64 i = lo; i < hi; ++i) w[i] = ~(u64)(uintptr_t)&w[i]; });
            check_pass(r, name, [w](u64 i) { return ~(u64)(uintptr_t)&w[i]; }, no_rewrite);
            break;
        }
        case MP_WALK1:
        case MP_WALK0: {
            const u64 flip = p == MP_WALK0 ? ~0ull : 0ull;
            auto want = [pass, flip](u64 i) { return (1ull << ((i + pass) & 63)) ^ flip; };
            for (u64 i = 0; i < n; ++i) w[i] = want(i);
            maybe_inject(r);
            check_pass(r, name, want, no_rewrite);
            break;
        }
        case MP_RANDOM: {
            // xorshift64 stream; the check regenerates it in the same order.
            const u64 seed = stab_mix(pass * 0x1000 + (u64)r->chunk) | 1;
            u64 x = seed;
            for (u64 i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; w[i] = x; }
            maybe_inject(r);
            x = seed;
            for (u64 b0 = 0; b0 < n; b0 += BLOCK) {
                const u64 hi = b0 + BLOCK < n ? b0 + BLOCK : n;
                for (u64 i = b0; i < hi; ++i) {
                    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                    if (w[i] != x) report(r, i, name, x);
                }
                if ((b0 & 0xFFFF) == 0 && g_stab.stop.load(std::memory_order_relaxed)) return;
            }
            r->bytes_checked += (f64)n * 8;
            break;
        }
        case MP_INVERSIONS: {
            // MemTest86-style moving inversions: fill P; ascending check P / write ~P; descending (block by block)
            // check ~P / write P; check P.
            const u64 P = (pass & 1) ? stab_mix(pass) : 0x5555555555555555ull;
            for (u64 i = 0; i < n; ++i) w[i] = P;
            maybe_inject(r);
            check_pass(r, name, [P](u64) { return P; },
                       [w, P](u64 lo, u64 hi) { for (u64 i = lo; i < hi; ++i) w[i] = ~P; });
            check_pass(r, name, [P](u64) { return ~P; },
                       [w, P](u64 lo, u64 hi) { for (u64 i = lo; i < hi; ++i) w[i] = P; }, true);
            check_pass(r, name, [P](u64) { return P; }, no_rewrite);
            break;
        }
        default: break;
    }
}

static void mem_worker(u32 index, void* user) {
    MemShared* s = (MemShared*)user;
    Region r = { s, s->base + (u64)index * s->region_words, s->region_words, (i32)s->lps[index], (i32)index, 0.0 };
    for (u64 pass = 0; !stab_done(s->deadline); ++pass) {
        for (u32 p = 0; p < MP_COUNT && !stab_done(s->deadline); ++p) {
            if (index == 0) stab_phase(STAB_MEM, PATTERN_NAME[p]);
            run_pattern(&r, (MemPattern)p, pass);
            stab_work_mt(STAB_MEM, r.bytes_checked / 1e9);
            r.bytes_checked = 0.0;
        }
    }
}

void stab_mem_run(f64 deadline) {
    StabTestState* ts = &g_stab.tests[STAB_MEM];
    const StabConfig* c = &g_stab.cfg;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    const u64 avail = ms.ullAvailPhys;
    u64 bytes = c->mem_bytes ? c->mem_bytes : avail / 100 * (c->mem_pct ? c->mem_pct : 70);
    bool capped = false;
    if (avail < HEADROOM + MIN_BYTES) {
        stab_fail_setup(STAB_MEM, "only %llu MiB of RAM available; the test keeps %llu MiB free for Windows",
                        avail / MiB, HEADROOM / MiB);
        return;
    }
    if (bytes > avail - HEADROOM) { bytes = avail - HEADROOM; capped = true; }
    if (bytes < MIN_BYTES) bytes = MIN_BYTES;
    u32 threads = g_topo.lp_count < STAB_MAX_LP ? g_topo.lp_count : STAB_MAX_LP;
    const u64 region = (bytes / threads) & ~(2 * MiB - 1);     // whole large-page-sized steps per region
    if (!region) { stab_fail_setup(STAB_MEM, "test size too small"); return; }
    bytes = region * threads;

    stab_phase(STAB_MEM, "Allocating and locking");
    // Locking needs the working-set minimum to cover the buffer; restore the old limits afterwards.
    HANDLE proc = GetCurrentProcess();
    SIZE_T ws_min = 0, ws_max = 0;
    DWORD ws_flags = 0;
    const bool have_ws = GetProcessWorkingSetSizeEx(proc, &ws_min, &ws_max, &ws_flags) != 0;
    u64* base = (u64*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base) {
        stab_fail_setup(STAB_MEM, "cannot allocate %llu MiB (error %lu)", bytes / MiB, GetLastError());
        return;
    }
    bool locked = false;
    DWORD lock_err = 0;
    if (have_ws && SetProcessWorkingSetSizeEx(proc, ws_min + bytes + 64 * MiB, ws_max + bytes + 256 * MiB, 0))
        locked = VirtualLock(base, bytes) != 0;
    if (!locked) {
        lock_err = GetLastError();
        for (u64 off = 0; off < bytes / 8; off += 512) base[off] = 0;     // pre-fault anyway
    }

    MemShared s;
    s.base = base;
    s.region_words = region / 8;
    s.threads = threads;
    u32 lps[STAB_MAX_LP];
    for (u32 i = 0; i < threads; ++i) lps[i] = g_topo.order[i];
    s.lps = lps;
    s.deadline = deadline;
    s.start = timer_now();
    s.injected.store(false);
    ts->size_bytes = bytes;
    ts->chunk_count = threads;
    ts->chunk_bytes = region;
    const f64 pct = 100.0 * (f64)bytes / (f64)(g_sys.ram.installed_bytes ? g_sys.ram.installed_bytes : ms.ullTotalPhys);
    if (locked)
        snprintf(ts->note, sizeof(ts->note), "%llu MiB locked (%.0f%% of installed RAM)%s", bytes / MiB, pct,
                 capped ? ", capped to keep 2 GiB free" : "");
    else
        snprintf(ts->note, sizeof(ts->note), "%llu MiB (%.0f%% of installed RAM), not locked (error %lu): Windows may "
                 "page parts of it out", bytes / MiB, pct, lock_err);

    stab_run_threads(threads, lps, mem_worker, &s);

    if (locked) VirtualUnlock(base, bytes);
    VirtualFree(base, 0, MEM_RELEASE);
    if (have_ws) SetProcessWorkingSetSizeEx(proc, ws_min, ws_max, ws_flags);
}
