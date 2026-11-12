#pragma once
// Core benchmark types and the static registry.
#include "core/types.h"
#include <atomic>

enum SuiteId : u8 { SUITE_SMOKE, SUITE_CPU, SUITE_MEM, SUITE_GPU_VK, SUITE_GPU_D3D12, SUITE_DISK, SUITE_COUNT };
enum ResultKind : u8 { RESULT_VALUES, RESULT_SERIES, RESULT_MATRIX };
enum BenchStatus : u8 { ST_PENDING, ST_RUNNING, ST_DONE, ST_SKIPPED, ST_FAILED, ST_CANCELLED, ST_NOT_RUN };

struct Metric {
    const char* name;   // static string
    const char* unit;   // static string
    f64 value;
    f64 x;              // SERIES: x coordinate (in BenchResult::x_unit)
};

struct BenchResult {
    u32 first, count;               // slice into the metric arena (results.h)
    ResultKind kind;
    u16 rows, cols;                 // MATRIX dims, values row-major
    const char* x_name;             // SERIES: x axis name
    const char* x_unit;             // SERIES: "bytes", "threads", ...
    f64 seconds;                    // wall time of the test
    bool skipped;                   // set by res_skip (e.g. unsupported ISA)
    char error[128];                // reason when FAILED / SKIPPED
};

// Disk suite options beyond the CrystalDiskMark basics (src/disk/, docs/notes/disk-bench.md).
enum DiskMode : u8 { DISK_MODE_RW, DISK_MODE_READ, DISK_MODE_WRITE, DISK_MODE_RW_MIX };
enum DiskData : u8 { DISK_DATA_RANDOM, DISK_DATA_ZERO };            // test data: incompressible or all 0x00
enum DiskUnit : u8 { DISK_UNIT_MBS, DISK_UNIT_GBS, DISK_UNIT_IOPS, DISK_UNIT_US };   // GUI display only
constexpr u8 DISK_ROW_RW = 255;     // DiskRow::read_pct: separate read and write passes (follows DiskOptions::mode)
struct DiskRow {                    // user-defined test row, e.g. "RND4K Q32T4 R70"
    u32 block;                      // bytes; 0 = row not used
    u16 qd, threads;
    u8 read_pct;                    // DISK_ROW_RW, or 0..100 % reads in one mixed pass (100 = read only)
    bool random;
};
constexpr u32 DISK_CUSTOM_ROWS = 4;
struct DiskOptions {
    u8 mode = DISK_MODE_RW;         // which passes the CrystalDiskMark rows run
    u8 mix_read_pct = 70;           // DISK_MODE_RW_MIX: reads in the mixed pass
    u8 data = DISK_DATA_RANDOM;
    u8 unit = DISK_UNIT_MBS;
    bool buffered = false;          // through the OS file cache (e.g. network shares)
    u32 interval_ms = 0;            // pause before each disk test after the first (CrystalDiskMark "Interval")
    u32 sustained_gib = 64;         // cap of the sustained-write test (also at most 80 % of the free space)
    wchar_t folder[260] = {};       // test in this folder instead of on the drive letters ("" = drives)
    DiskRow custom[DISK_CUSTOM_ROWS] = {};
};

struct BenchParams {
    u32 duration_ms = 3000;         // per measured phase
    u32 thread_count = 0;           // 0 = all logical processors
    u32 gpu_adapter = 0;            // index into SysInfo::gpus
    u32 gpu_adapters = 0;           // bit n = SysInfo::gpus[n]: the GPU suites run on each; 0 = gpu_adapter only
    u32 gpu_width = 1920, gpu_height = 1080;
    bool gpu_fullscreen = true;     // on-screen mode: borderless fullscreen, else a gpu_width x gpu_height window
    bool gpu_onscreen = false;      // 3D / 2D tests present to a window (gpu/gpu_present.h); default offscreen
    u32 disk_drives = 1u << ('C' - 'A');    // bit n = drive letter 'A' + n; the disk suite runs on each

    u64 disk_file_bytes = 1 * 1024ull * 1024 * 1024;
    u32 disk_passes = 3;            // best of N (CrystalDiskMark uses 5; 3 is as stable here with ~40% less wear)
    bool disk_write_through = false;
    u32 repeat = 1;                 // each test runs this many times in a row; results are the median (runner.cpp)
    u32 sustain_s = 0;              // sustained mode: each test loops for this long (>= 2 passes), median + per-pass
                                    // series; 0 = off. Overrides `repeat` (runner.cpp)
    DiskOptions disk;
};
constexpr u32 MAX_REPEAT = 10;
constexpr u32 MAX_SUSTAIN_S = 60 * 60;

// Live state of the running test. Single writer (worker), single reader (UI / CLI).
constexpr u32 LIVE_HISTORY = 256;
struct BenchLive {
    std::atomic<f32> progress;                  // 0..1 within the current test
    std::atomic<f64> current;                   // latest instantaneous value
    std::atomic<const char*> unit;              // unit of `current`
    std::atomic<const char*> phase;             // e.g. "Warm-up", "Read"
    std::atomic<f32> history[LIVE_HISTORY];     // ring buffer for the sparkline
    std::atomic<u32> history_count;             // total samples pushed (head = count % LIVE_HISTORY)
};

void live_reset(BenchLive* live);
void live_sample(BenchLive* live, f64 value);   // sets current + pushes history
inline void live_progress(BenchLive* live, f32 p) { live->progress.store(p, std::memory_order_relaxed); }
inline void live_phase(BenchLive* live, const char* phase, const char* unit) {
    live->phase.store(phase, std::memory_order_relaxed);
    live->unit.store(unit, std::memory_order_relaxed);
}

typedef bool (*SuiteInitFn)(const BenchParams* p, void** state, char* error, u32 error_size);
typedef void (*SuiteShutdownFn)(void* state);
typedef void (*BenchFn)(void* state, const BenchParams* p, BenchLive* live, BenchResult* out,
                        const std::atomic<bool>* cancel);

struct SuiteDesc {
    SuiteId id;
    const char* name;       // UI name
    const char* cli_id;     // e.g. "cpu"
    SuiteInitFn init;       // may be null
    SuiteShutdownFn shutdown;
};

enum BenchFlags : u32 {
    BENCH_DEFAULT_OFF = 1u << 0,    // not selected by default in the GUI
    BENCH_PREVIEW     = 1u << 1,    // publishes a rendered preview (GPU 3D / 2D) shown in the live panel
    BENCH_CONFIRM     = 1u << 4,    // writes a lot or runs very long: the GUI asks first, the CLI needs --yes,
                                    // presets never select it
};

struct BenchDesc {
    SuiteId suite;
    const char* group;      // UI group name, e.g. "Compute"
    const char* name;       // UI test name
    const char* cli_id;     // full id, e.g. "cpu.fma.avx2"
    BenchFn fn;
    u32 flags;
};

extern const SuiteDesc g_suites[SUITE_COUNT];
extern const BenchDesc g_benches[];
extern const u32 g_bench_count;
constexpr u32 MAX_BENCHES = 256;
