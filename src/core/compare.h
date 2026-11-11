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

// Run vs the reference run over the metrics both have, as performance change: ratios are flipped for
// lower-is-better units (latency, frame time) so + always means faster / better. Leaves out ratios (unit
// "x" / "%"), matrix min / max rows, "Theoretical ..." constants and p99.9 frame times (too noisy).
struct CmpSummary {
    f64 geomean_pct;                    // geometric mean of the performance ratios, as a % change
    u32 compared, better, worse, same;  // per-metric counts, "same" = within 1%
    f64 best_pct, worst_pct;            // largest gain / loss
    u32 best_row, worst_row;
};
// `suite` limits it to one suite (null = all). False if the two runs share no metric.
bool compare_summary(u32 run, const char* suite, CmpSummary* out);

// Adds a saved results file as the last column. False (with `err`) if it can't be read or the set is full.
bool compare_add_file(const wchar_t* path, char* err, u32 err_size);
// Same for results text already in memory (e.g. the built-in library), labelled `label`.
bool compare_add_buffer(const char* label, const char* data, u32 size, char* err, u32 err_size);
// Reads only the system info of a results text into `out` (info[]; label left empty). Nothing is added.
bool compare_peek(const char* data, u32 size, CmpRun* out, char* err, u32 err_size);
bool compare_has_label(const char* label);
// Adds the finished tests of the last run in this process. False if there is nothing to add.
bool compare_add_current(char* err, u32 err_size);
void compare_remove(u32 run);
void compare_clear();
