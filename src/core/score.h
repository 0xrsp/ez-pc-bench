#pragma once
// Overall score (FUTURE.md section 10): a derived, published, versioned number computed only from the raw
// results, so any results file (old ones too) gets one. Raw numbers stay the primary output.
//
//   ratio_i     = value_i / ref_i            (ref_i / value_i for lower-is-better metrics)
//   component_c = 1000 * exp(sum_i w_i ln ratio_i / sum_i w_i)        over the inputs of c that are present
//   overall     = 1000 * exp(sum_c W_c ln(component_c / 1000) / sum_c W_c)     only when every input is present
//
// The reference run scores exactly 1000 everywhere. Inputs, weights, directions and the frozen reference
// values live in one static table per version (score.cpp); a new version only changes data. Scores of
// different versions are never compared.
#include "core/runfile.h"
#include <stdio.h>

enum ScoreComp : u8 { SC_CPU_ST, SC_CPU_MT, SC_MEM, SC_GPU, SC_DISK };
constexpr u32 SC_COUNT = 5;

enum ScoreInputFlags : u8 {
    SI_DRIVE = 1u << 0,     // disk metric named without its "C: " prefix; uses C: if tested, else the first drive
};

struct ScoreInput {
    ScoreComp comp;
    const char* test;       // test id; '*' wildcard = best of the matches ("gpu_*.3d.scene": Vulkan or D3D12)
    const char* metric;     // metric name, '*' wildcard allowed
    f64 x;                  // series x (bytes), 0 for plain values
    bool lower_better;
    f32 weight;             // within its component
    f64 ref;                // value in the reference run, frozen for this version
    const char* unit;
    const char* label;      // short name for the UI / CLI
    u8 flags;               // ScoreInputFlags
};

struct ScoreVersion {
    u32 version;
    const char* status;             // "preliminary" until calibrated (FUTURE.md section 10)
    const char* reference;          // reference run (library file name without .toon.txt)
    const char* reference_desc;
    const ScoreInput* inputs;
    u32 input_count;
    f32 overall_weights[SC_COUNT];  // % per component
    const char* calibration;        // calibration state shown on the About page
};
extern const ScoreVersion g_score;  // the current version

extern const char* const g_score_comp_names[SC_COUNT];      // "CPU single-thread", ...
extern const char* const g_score_comp_ids[SC_COUNT];        // "cpu_st", ... (results file)

// Every test score v1 reads; the Score preset (presets.cpp) runs exactly these at default settings.
#define SCORE_V1_TESTS                                                                                        \
    "cpu.int.alu,cpu.int.mul,cpu.fp.scalar,cpu.fp.avx2_f32,cpu.fp.sgemm,cpu.crypto.sha256,cpu.crypto.aes,"     \
    "cpu.work.sort,cpu.work.sieve,mem.bw.read,mem.bw.copy,mem.lat.chase,"                                     \
    "disk.seq1m_q8t1,disk.seq1m_q1t1,disk.rnd4k_q32t1,disk.rnd4k_q1t1,"                                       \
    "gpu_vk.compute.fp32,gpu_vk.compute.vram,gpu_vk.3d.triangles,gpu_vk.3d.fill,gpu_vk.3d.texfill,gpu_vk.3d.scene," \
    "gpu_d3d12.compute.fp32,gpu_d3d12.compute.vram,gpu_d3d12.3d.triangles,gpu_d3d12.3d.fill,gpu_d3d12.3d.texfill," \
    "gpu_d3d12.3d.scene"

// Validity: never hides the score, flags what makes it less comparable.
enum ScoreFlags : u32 {
    SF_INCOMPLETE    = 1u << 0,     // some inputs missing: component scores only
    SF_SETTINGS      = 1u << 1,     // not the default settings (phase length, threads, resolution, disk file)
    SF_BATTERY       = 1u << 2,
    SF_POWER_MODE    = 1u << 3,     // Windows power mode not "Best performance"
    SF_THROTTLE      = 1u << 4,     // clock drop / hot CPU or GPU during an input test
    SF_BACKGROUND    = 1u << 5,     // CPU busy beyond the test's own thread during single-thread tests
    SF_MEM_INTEGRITY = 1u << 6,     // memory integrity (HVCI) on
    SF_NOT_ADMIN     = 1u << 7,
    SF_LARGE_PAGES   = 1u << 8,     // memory latency measured with 2 MiB pages (the reference used 4 KiB)
    SF_NOISY         = 1u << 9,     // an input's spread over repeats >= 3 %
    SF_NO_CONDITIONS = 1u << 10,    // file without recorded conditions (before M7): battery / throttling unknown
};
constexpr u32 SF_COUNT = 11;
extern const char* const g_score_flag_ids[SF_COUNT];        // results file names
extern const char* const g_score_flag_desc[SF_COUNT];       // one line each

constexpr u32 SCORE_MAX_INPUTS = 64;

struct ScoreInputValue {
    bool present;
    char test[40];          // the test the value came from (best match of a wildcard)
    f64 value;
    f64 ratio;              // performance vs the reference (> 1 = faster)
};

struct ScoreResult {
    u32 version;
    bool any;               // at least one input present
    bool complete;          // every input present: overall exists
    char drive;             // disk letter the storage inputs came from, 0 if none
    u32 flags;              // ScoreFlags
    f64 comp[SC_COUNT];     // NaN when the component has no input
    f32 coverage[SC_COUNT]; // present weight / total weight (1 = all inputs)
    f64 overall;            // NaN unless complete
    ScoreInputValue in[SCORE_MAX_INPUTS];
};

void score_compute(const RunFile* rf, ScoreResult* out);
bool score_from_text(const char* data, u32 size, ScoreResult* out);  // false: not a results file
void score_current(ScoreResult* out);                               // the run in memory
void score_flag_text(u32 flags, char* buf, u32 size);               // "on battery, power mode" / ""
void score_inputs_of(ScoreComp c, u32* present, u32* total, const ScoreResult* s);
// Overall and the components as one list (pickers, table rows): item 0 = Overall, 1 + c = component c.
constexpr u32 SCORE_ITEM_COUNT = 1 + SC_COUNT;
const char* score_item_name(u32 item);
f64 score_item(const ScoreResult* s, u32 item);
// `score:` block of a results file (derived values with the version; readers recompute from the raw data).
void score_write_toon(FILE* f, const ScoreResult* s);
// Plain-text breakdown (CLI --score).
void score_print(FILE* f, const ScoreResult* s);
// Recomputes a saved file's score and replaces (or adds) its `score:` block, before `integrity:` if present.
bool score_rewrite_file(const wchar_t* path, char* err, u32 err_size);
