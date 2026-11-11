#pragma once
// Flat view of one results run, read from a TOON results file (any age: pre-M7 files have no env / windows
// blocks) or taken from the run in memory. Used by the score, history and ranking; the compare window keeps
// its own aligned table (compare.h).
#include "core/types.h"

constexpr u32 RF_MAX_METRICS = 4096;
constexpr u32 RF_MAX_TESTS = 256;

struct RfTest {
    char id[48];
    u32 env_flags;          // EnvFlags (monitor.h)
    f32 cpu_util;           // % of all logical processors, NaN if not recorded
    bool has_env;
};

struct RfMetric {
    u16 test;               // index into RunFile::tests
    char name[64];
    char unit[16];
    f64 value;
    f64 x;                  // series x, matrix cell index, 0 for plain values (as in the file)
    f32 cv;                 // spread over repeats in %, NaN for single runs
};

struct RunFile {
    char kind[24];          // top-level "kind" ("" for benchmark runs; e.g. "stability")
    char timestamp[32];     // "2026-11-11T11:38:11"
    char cpu[96], gpu[128], os[64];
    char ram_type[16];
    u32 ram_mts, cpu_threads;
    f64 ram_bytes;
    bool on_ac, elevated;
    char power_mode[48];
    i8 memory_integrity;    // -1 unknown (older files)
    // params (has_params false for files without them)
    bool has_params;
    f64 duration_ms, thread_count, gpu_width, gpu_height, disk_file_bytes, disk_passes, repeat;
    bool disk_write_through;
    u32 disk_drives;        // bit n = drive 'A' + n
    bool has_env;           // the run recorded conditions (env table)
    u32 test_count;
    RfTest tests[RF_MAX_TESTS];
    u32 metric_count;
    RfMetric metrics[RF_MAX_METRICS];
};

// Parses a results text. Lenient: unknown keys and tables are skipped. False (with `err`) only if it isn't an
// ez-pc-bench results file at all.
bool rf_parse(const char* data, u32 size, RunFile* out, char* err, u32 err_size);
// The finished tests of the last run in this process.
void rf_from_current(RunFile* out);
// One shared scratch instance (UI thread / CLI main thread only): RunFile is ~400 KB.
RunFile* rf_scratch();

// '*' matches any run of characters.
bool rf_wild(const char* pattern, const char* s);
i32 rf_find_test(const RunFile* rf, const char* id);
// Metric `name` (wildcards allowed) of test `test_index` at x, or -1.
i32 rf_find_metric(const RunFile* rf, u32 test_index, const char* name, f64 x);
