#pragma once
// Disk suite helpers for the GUI and CLI: mode names, display units and the notes shown before tests that
// write a lot.
#include "core/bench.h"
#include <stdio.h>

const char* disk_mode_name(u8 mode);                // "rw", "read", "write", "mix"
bool disk_mode_parse(const char* s, u8* mode);

// Display units (DiskOptions::unit): which metric the GUI shows first for a disk test, and MB/s -> GB/s.
// Returns the index of the metric to summarize a result with.
u32 disk_unit_pick(u8 unit, const BenchResult* r);
f64 disk_unit_value(u8 unit, const char* metric_unit, f64 v, const char** shown_unit);

// For BENCH_CONFIRM tests: what the test will do, e.g. "writes up to 64 GiB to each tested drive".
void disk_confirm_note(const BenchParams* p, u32 bench, char* buf, u32 size);

// Results file: the DiskOptions as `params:` entries (disk_mode, disk_mix_read_pct, disk_buffered, ...).
void disk_write_toon_params(FILE* f, const BenchParams* p);
