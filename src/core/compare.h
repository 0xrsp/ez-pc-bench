#pragma once
// Comparison set: several result runs (saved TOON files and/or the current in-memory run) aligned into one
// table. Rows are (test, metric, x) in test order; values[row][run] is NaN where a run lacks that metric.
// Matrix results are reduced to three rows (off-diagonal min / avg / max).
#include "core/bench.h"

constexpr u32 CMP_MAX_RUNS = 16;
constexpr u32 CMP_MAX_TESTS = 256;
constexpr u32 CMP_MAX_ROWS = 4096;

// System / settings fields shown side by side above the results.
enum CmpInfo : u8 { CI_DATE, CI_CPU, CI_RAM, CI_GPU, CI_DISK, CI_OS, CI_POWER, CI_ADMIN, CI_DURATION, CI_COUNT };
extern const char* const g_cmp_info_names[CI_COUNT];

struct CmpRun {
    char label[96];             // file name without ".toon.txt", or "Current run"
    char info[CI_COUNT][128];
    u32 metrics;                // rows this run has a value for
};

struct CmpTest {
    char id[48];
    char suite[32];
    char title[96];             // "Suite / Name"
    char x_unit[16];            // series x unit ("bytes", "threads")
};

enum CmpStat : u8 { CS_VALUE, CS_MIN, CS_AVG, CS_MAX };     // CS_MIN.. = matrix summary rows

struct CmpRow {
    u16 test;                   // index into tests
    CmpStat stat;
    char name[64];
    char unit[16];
    f64 x;                      // series x (0 otherwise)
};

struct CompareSet {
    CmpRun runs[CMP_MAX_RUNS];
    u32 run_count;
    u32 reference;              // run the others are compared against
    CmpTest tests[CMP_MAX_TESTS];
    u32 test_count;
    CmpRow rows[CMP_MAX_ROWS];
    u32 row_count;
    f64 values[CMP_MAX_ROWS][CMP_MAX_RUNS];
};
extern CompareSet g_cmp;

// Adds a saved results file as the last column. False (with `err`) if it can't be read or the set is full.
bool compare_add_file(const wchar_t* path, char* err, u32 err_size);
// Adds the finished tests of the last run in this process. False if there is nothing to add.
bool compare_add_current(char* err, u32 err_size);
void compare_remove(u32 run);
void compare_clear();
