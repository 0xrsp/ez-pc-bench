#pragma once
// Baseline for comparisons: results read back from one of our own JSON exports.
#include "core/bench.h"

struct Baseline {
    bool loaded;
    char file[260];         // file name without directory (UTF-8)
    char timestamp[32];     // "timestamp" field of the export
    u32 count;              // metrics loaded
};
extern Baseline g_baseline;

// Replaces the current baseline. On failure the baseline is cleared and `err` says why.
bool baseline_load(const wchar_t* path, char* err, u32 err_size);
void baseline_clear();

// Lookup key of metric k: series x, matrix cell index, 0 for plain values (matched by name).
f64 baseline_key_x(const BenchResult* r, u32 k);
bool baseline_find(const char* test_id, const char* metric, f64 key_x, f64* value);
// Baseline value of metric k of a finished result.
bool baseline_metric(const char* test_id, const BenchResult* r, u32 k, f64* value);
// Average over the off-diagonal cells of a matrix result (also min/max). False if no cell is in the baseline.
bool baseline_matrix(const char* test_id, const BenchResult* r, f64* lo, f64* avg, f64* hi);

// Latency / time units (ns, µs, us, ms, s): smaller is better. Everything else: bigger is better.
bool unit_lower_is_better(const char* unit);
f64 delta_pct(f64 value, f64 base);                         // signed percent change vs base
i32 delta_rating(f64 pct, const char* unit);                // +1 better, -1 worse, 0 within 1%
void format_delta(char* buf, u32 size, f64 pct);            // "+1.2%", "-0.4%"
