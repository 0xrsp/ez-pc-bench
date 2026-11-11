#pragma once
// Disk health persistence under %APPDATA%\ez-pc-bench\health\:
//   health.ini            per-drive health thresholds ([<drive key>]) and window options ([ui])
//   <drive key>.toon.txt  history samples (TOON: drive object + samples[N]{...} table), newest last
// The drive key is the model (file-name safe) plus a hash of model + serial, so the serial itself never
// appears in a file name.
#include "core/types.h"

// Caution rules a user can tune per drive (CrystalDiskInfo-style). 0 turns a rule off.
struct HealthThresholds {
    u32 reallocated;        // ATA 05: raw count >= this -> Caution
    u32 pending;            // ATA C5
    u32 uncorrectable;      // ATA C6
    u32 media_errors;       // NVMe media and data integrity errors
    u32 life_left_pct;      // remaining life <= this % -> Caution
};
constexpr HealthThresholds HEALTH_THRESHOLDS_DEFAULT = { 1, 1, 1, 1, 10 };

void health_drive_key(const char* model, const char* serial, char* out, u32 size);
void health_thresholds_load(const char* key, HealthThresholds* out);   // defaults where unset
void health_thresholds_save(const char* key, const HealthThresholds* t);
bool health_ui_option(const char* name, bool def);
void health_set_ui_option(const char* name, bool value);

// One history sample. NaN = not reported by this drive.
enum HealthField : u8 {
    HF_TEMP_C, HF_LIFE_USED, HF_WRITTEN_GB, HF_READ_GB, HF_POWER_ON_H, HF_REALLOCATED, HF_PENDING, HF_UNCORRECTABLE,
    HF_MEDIA_ERRORS, HF_COUNT
};
extern const char* const HEALTH_FIELD_IDS[HF_COUNT];     // TOON column names
extern const char* const HEALTH_FIELD_NAMES[HF_COUNT];   // UI labels
extern const char* const HEALTH_FIELD_UNITS[HF_COUNT];

struct HealthSample {
    i64 time;               // Unix seconds
    f64 v[HF_COUNT];
};
constexpr u32 HEALTH_HISTORY_MAX = 4096;   // oldest samples are dropped beyond this

// Loads the history of `key` (oldest first). Returns the sample count.
u32 health_history_load(const char* key, HealthSample* out, u32 max);
// Appends `s` when the last sample is >= 1 hour old or a wear / error counter changed. True if written.
bool health_history_add(const char* key, const char* model, const HealthSample* s);
// Folder path for "open folder" (UTF-16).
const wchar_t* health_store_dir();

// Endurance projection from the history: needs samples spanning at least a day.
struct HealthProjection {
    f64 days;               // span of the samples used
    f64 written_gb_per_day; // NaN if unknown
    f64 life_pct_per_day;   // NaN if unknown or 0 change
    i64 life_end_time;      // estimated time of 100 % life used (0 = not enough data)
};
bool health_project(const HealthSample* s, u32 n, HealthProjection* out);
