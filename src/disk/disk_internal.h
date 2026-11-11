#pragma once
// Shared by the disk suite files: test targets (disk_suite.cpp), latency histograms and the overlapped
// I/O engine (disk_io.cpp), result helpers. Public parts for the GUI / CLI are in disk.h.
#include "core/bench.h"
#include "core/results.h"
#include <windows.h>

typedef std::atomic<bool> CancelFlag;

constexpr u32 SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr u32 DISK_FOLDER_SLOT = 26;            // target slot of the folder target (0..25 = drive letters)
constexpr u32 DISK_MAX_TARGETS = 27;

// ---- targets (disk_suite.cpp) -----------------------------------------------------------------------
// One test file per selected drive, or one in the folder of DiskOptions::folder.

struct DiskTarget {
    u32 slot;                       // 0..25 = drive letter, DISK_FOLDER_SLOT = folder
    char label[8];                  // metric name prefix: "C:" or "Folder"
    wchar_t letter;                 // drive letter of the volume, 0 for UNC paths
    wchar_t path[MAX_PATH];         // the suite's test file
    wchar_t work_dir[MAX_PATH];     // ...\ezb_work: extra files of the metadata / copy / sustained tests
    wchar_t made_dir[MAX_PATH];     // "<drive>:\ezbench" when this run created it (removed at shutdown)
    HANDLE hold;                    // fill handle, kept open; DELETE_ON_CLOSE also cleans up after a crash
    u32 sector;                     // alignment for unbuffered I/O (larger of logical / physical)
    u32 logical_sector;
    i32 phys_disk;                  // N of \\.\PhysicalDriveN, -1 if unknown
    bool rotational;                // seek penalty (HDD)
};

struct DiskSuite {
    DiskTarget targets[DISK_MAX_TARGETS];
    u32 count;
    u64 file_bytes;
    bool write_through, buffered;
    u8 data;                        // DiskData
    u32 tests_started;              // for the interval between tests
};
extern DiskSuite g_disk;

// Interned, never freed string ("C: Read p99"): metric and phase names must outlive the suite.
const char* dname(const char* fmt, ...);

// Called first by every disk test: checks there is a target and waits the configured interval.
bool disk_begin_test(const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel);
void pause_ms(u32 ms, const CancelFlag* cancel);
bool cancelled(const CancelFlag* cancel);

u64 rng_next(u64* s);
// Test data: zero_pct % of every 4 KiB is 0x00, the rest random (0 = incompressible, 100 = all zero).
void fill_pattern(u8* buf, u64 bytes, u32 zero_pct, u64 seed);
u32 disk_zero_pct();                // from DiskOptions::data
void* disk_alloc(u64 bytes);        // page-aligned, zeroed; disk_free
void disk_free(void* p);

// Drive temperature of the target's physical disk (NVMe composite / SATA sensor). False if unavailable.
bool disk_temperature(const DiskTarget* t, f32* temp_c);
// "<work_dir>\name"; creates work_dir on first use.
bool disk_work_path(const DiskTarget* t, const wchar_t* name, wchar_t* out);
void disk_delete_tree(const wchar_t* dir);     // files and subfolders below dir (our own work folders only)

// ---- latency histogram (disk_io.cpp) ------------------------------------------------------------------
// Fixed log-linear buckets over QPC ticks: exact below 32 ticks, then 16 buckets per power of two (~4 %).
// Filling it is a few instructions and never allocates, so every I/O of every test is recorded.

constexpr u32 HIST_SUB_BITS = 4;
constexpr u32 HIST_SUB = 1u << HIST_SUB_BITS;
constexpr u32 HIST_TOP_BIT = 40;                // values clamp below 2^40 ticks (~30 h)
constexpr u32 HIST_BUCKETS = 2 * HIST_SUB + (HIST_TOP_BIT - HIST_SUB_BITS - 1) * HIST_SUB;

struct LatHist {
    u64 count, sum_ticks, max_ticks;
    u64 buckets[HIST_BUCKETS];
};

inline u32 hist_bucket(u64 v) {
    if (v < 2 * HIST_SUB) return (u32)v;
    if (v >= (1ull << HIST_TOP_BIT)) v = (1ull << HIST_TOP_BIT) - 1;
    unsigned long e;
    _BitScanReverse64(&e, v);
    return 2 * HIST_SUB + (e - HIST_SUB_BITS - 1) * HIST_SUB + (u32)((v >> (e - HIST_SUB_BITS)) & (HIST_SUB - 1));
}
inline void hist_add(LatHist* h, u64 ticks) {
    ++h->count;
    h->sum_ticks += ticks;
    if (ticks > h->max_ticks) h->max_ticks = ticks;
    ++h->buckets[hist_bucket(ticks)];
}
void hist_reset(LatHist* h);
void hist_merge(LatHist* dst, const LatHist* src);
f64 hist_bucket_upper_us(u32 b);                // upper edge of a bucket in microseconds
f64 hist_percentile_us(const LatHist* h, f64 pct);  // pct 0..100; 100 = exact max
f64 hist_avg_us(const LatHist* h);
f64 ticks_us(u64 ticks);

// ---- I/O engine (disk_io.cpp) ---------------------------------------------------------------------------
// Unbuffered overlapped I/O on the target's test file: per thread one handle, one completion port and `qd`
// requests in flight, resubmitted on completion. Several groups can run at once (e.g. a QD1 reader next to
// sequential writers). Each I/O is timed with QPC from submit to completion into per-thread histograms.

constexpr u32 IO_MAX_QD = 256;
constexpr u32 IO_MAX_WORKERS = 64;

struct IoSpec {
    u32 block;                      // bytes, multiple of the sector
    u32 qd;                         // requests in flight per thread (1..IO_MAX_QD)
    u32 threads;
    bool random;
    u8 read_pct;                    // 100 = reads, 0 = writes, else each request is a read with this chance
    bool flush;                     // FlushFileBuffers after every write, counted in its latency (use QD1)
    bool write_through;             // open with FILE_FLAG_WRITE_THROUGH (writes are FUA)
    u64 region_off, region_bytes;   // part of the file to use (region_bytes 0 = all of it)
    u64 max_ios;                    // per thread; 0 = run for IoRun::seconds. Bounded runs end when all threads are done
};

struct IoResult {
    u64 ios[2];                     // completed reads, writes
    u64 bytes;
    f64 seconds;
    LatHist hist[2];                // read, write latencies
};
inline f64 io_mbps(const IoResult* r) { return r->seconds > 0.0 ? r->bytes / r->seconds / 1e6 : 0.0; }
inline f64 io_iops(const IoResult* r) { return r->seconds > 0.0 ? (r->ios[0] + r->ios[1]) / r->seconds : 0.0; }

enum LiveKind : u8 { LIVE_MBS, LIVE_IOPS, LIVE_LAT_US };

struct IoRun {
    const IoSpec* groups;
    u32 group_count;
    f64 seconds;
    u32 live_group;                 // group shown in the live panel
    LiveKind live;
    u32 zero_pct;                   // write data (fill_pattern); disk_zero_pct() for the user's setting
    f32 prog_from, prog_to;
};

// Runs all groups for run->seconds; out has group_count entries. False on an I/O error (message in error).
bool io_run(const DiskTarget* t, const IoRun* run, IoResult* out, BenchLive* live, const CancelFlag* cancel,
            char* error, u32 error_size);
// One group, the user's test data.
bool io_run1(const DiskTarget* t, const IoSpec* spec, f64 seconds, LiveKind lk, f32 from, f32 to, IoResult* out,
             BenchLive* live, const CancelFlag* cancel, char* error, u32 error_size);

// ---- result helpers (disk_io.cpp) -------------------------------------------------------------------------

enum IoMetricFlags : u32 {
    IOM_THROUGHPUT = 1u << 0,       // "<p>" MB/s and "<p> IOPS"
    IOM_AVG        = 1u << 1,       // "<p> latency" (average)
    IOM_PCT        = 1u << 2,       // "<p> p50" .. "<p> p99.99", "<p> max"
    IOM_ALL        = IOM_THROUGHPUT | IOM_AVG | IOM_PCT,
};
// Adds metrics named "<label> <phase>...". dir: 0 reads, 1 writes, 2 both.
void add_io_metrics(BenchResult* out, const char* label, const char* phase, const IoResult* r, u32 dir, u32 flags);
// Same from a histogram alone (no throughput).
void add_hist_metrics(BenchResult* out, const char* label, const char* phase, const LatHist* h, u32 flags);

// A CrystalDiskMark-style row: best of N passes per direction on every target (disk_cdm.cpp).
struct RowSpec {
    u32 block, qd, threads;
    bool random;
    u8 read_pct;                    // DISK_ROW_RW = read / write (/ mix) passes by DiskOptions::mode
};
void disk_run_row(const RowSpec* row, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel);
