#pragma once
// Metric arena, per-bench result/status arrays, text + JSON export.
#include "core/bench.h"

constexpr u32 MAX_METRICS = 64 * 1024;

extern Metric g_metrics[MAX_METRICS];
extern BenchResult g_results[MAX_BENCHES];
extern std::atomic<u8> g_status[MAX_BENCHES];  // BenchStatus; DONE is stored with release order after the result

void results_reset();   // only while no run is active

// Called by benchmark functions to fill their result.
void res_begin(BenchResult* r, ResultKind kind);
void res_add(BenchResult* r, const char* name, const char* unit, f64 value, f64 x = 0.0);
void res_matrix(BenchResult* r, u16 rows, u16 cols);    // after adding rows*cols values
void res_series_axis(BenchResult* r, const char* x_name, const char* x_unit);
void res_fail(BenchResult* r, const char* fmt, ...);
void res_skip(BenchResult* r, const char* fmt, ...);

inline const Metric* res_metrics(const BenchResult* r) { return &g_metrics[r->first]; }
const char* status_name(BenchStatus s);

// Formats a value with sensible precision ("12.34", "1234", "0.0123").
void format_value(char* buf, u32 size, f64 v);
// Formats a series x value using its unit ("16 KiB" for bytes).
void format_x(char* buf, u32 size, f64 x, const char* x_unit);

// Plain-text report of all results (sysinfo header + table). Returns length written (truncated to size).
u32 results_format_text(char* buf, u32 size, const BenchParams* p);
bool results_write_json(const wchar_t* path, const BenchParams* p);
