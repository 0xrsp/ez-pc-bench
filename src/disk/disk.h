#pragma once
// Disk suite helpers for the GUI and CLI: CrystalDiskMark profiles, custom test rows, display units and the
// notes shown before tests that write a lot.
#include "core/bench.h"
#include <stdio.h>

struct DiskProfile {
    const char* id;         // CLI name
    const char* name;
    const char* desc;
    const char* ids;        // comma-separated test ids the profile selects
};
extern const DiskProfile g_disk_profiles[];
extern const u32 g_disk_profile_count;

const DiskProfile* disk_profile_find(const char* id);
// Selects exactly the profile's disk tests (other disk tests off); other suites are untouched.
void disk_profile_apply(const DiskProfile* pr, bool* selected);
const DiskProfile* disk_profile_match(const bool* selected);   // null = custom selection

// Custom rows as text: "<SEQ|RND><size>[K|M] Q<qd>T<threads> [R<read %>]", e.g. "RND4K Q32T4 R70".
// Without R the row runs read and write passes like the built-in rows; R100 = read only, R0 = write only.
bool disk_row_parse(const char* s, DiskRow* r);     // "" -> unused row (block 0), true
void disk_row_format(const DiskRow* r, char* buf, u32 size);
const char* disk_mode_name(u8 mode);                // "rw", "read", "write", "mix"
bool disk_mode_parse(const char* s, u8* mode);

// Display units (DiskOptions::unit): which metric the GUI shows first for a disk test, and MB/s -> GB/s.
// Returns the index of the metric to summarize a result with.
u32 disk_unit_pick(u8 unit, const BenchResult* r);
f64 disk_unit_value(u8 unit, const char* metric_unit, f64 v, const char** shown_unit);

// For BENCH_CONFIRM tests: what the test will do, e.g. "writes up to 64 GiB to each tested drive".
void disk_confirm_note(const BenchParams* p, u32 bench, char* buf, u32 size);

// Results file: the DiskOptions as `params:` entries (disk_mode, disk_mix_read_pct, disk_data, ...).
void disk_write_toon_params(FILE* f, const BenchParams* p);
