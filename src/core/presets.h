#pragma once
// One-click run presets (FUTURE.md section 11): a test selection plus the run settings that go with it.
// Sustained / thermal and Score presets come with the sustained mode (section 3) and the score (section 10).
#include "core/bench.h"

enum PresetSelect : u8 {
    PRESET_SEL_LIST,        // exactly the ids in `ids`
    PRESET_SEL_DEFAULT,     // every test not flagged BENCH_DEFAULT_OFF
    PRESET_SEL_ALL,         // every test (hidden suites still never run)
};

struct RunPreset {
    const char* id;         // CLI / ini name
    const char* name;
    const char* desc;       // tooltip / --help text
    PresetSelect select;
    const char* ids;        // PRESET_SEL_LIST: comma-separated test ids
    u32 duration_ms, repeat, disk_passes, disk_file_mib;
};

extern const RunPreset g_presets[];
extern const u32 g_preset_count;

const RunPreset* preset_find(const char* id);
// Applies the selection and the run settings; GPU / drives / threads / resolution stay as they are.
void preset_apply(const RunPreset* pr, BenchParams* p, bool* selected);
// The preset the current settings and selection match exactly, or null ("Custom").
const RunPreset* preset_match(const BenchParams* p, const bool* selected);
