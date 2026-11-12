#pragma once
// Stability tester (FUTURE.md section 6): looks for errors instead of measuring speed. Separate from the
// benchmark runner and its results: its own controller thread, live state (g_stab) and report file
// (TOON with `kind: stability`, see stability.cpp).
//
// Tests run in sequence (each for StabConfig::seconds) or together (combined load, all for `seconds`), or until
// stopped. Every test verifies what it computes or stores and counts mismatches; errors are attributed to the
// logical processor, memory region, VRAM chunk or file chunk where they happened. A 1 Hz monitor thread records
// clocks / load / temperatures and stops the run at the user's temperature limits. WHEA-Logger events are
// counted during the run (core/whea.h).
#include "core/types.h"
#include <atomic>

enum StabTest : u8 {
    STAB_CPU,       // self-checking CPU kernels on every logical processor
    STAB_CACHE,     // L1 / L2 / L3-sized pattern test on every logical processor
    STAB_CORES,     // one core at a time (CoreCycler-style)
    STAB_MEM,       // RAM pattern test
    STAB_VRAM,      // VRAM pattern test (D3D12)
    STAB_GPU,       // GPU compute + render with known answers (D3D12)
    STAB_POWER,     // maximum-power GPU load, unchecked except for device loss
    STAB_DISK,      // write / read back / compare a temporary file
    STAB_COUNT
};

struct StabTestDesc {
    const char* id;         // CLI id
    const char* name;
    const char* work_unit;  // unit of StabTestState::work
    const char* help;       // tooltip / --help text
};
extern const StabTestDesc g_stab_tests[STAB_COUNT];

// CPU test kernel. Mixed switches every few seconds (all threads at once, so the load steps too).
enum CpuKernel : u8 { CK_MIXED, CK_INT, CK_SSE, CK_AVX2, CK_AVX512, CK_LARGE, CK_COUNT };
extern const char* const g_cpu_kernel_ids[CK_COUNT];      // "mixed", "int", "sse", "avx2", "avx512", "large"
extern const char* const g_cpu_kernel_names[CK_COUNT];

struct StabConfig {
    bool run[STAB_COUNT] = {};
    bool together = false;          // run the selected tests at the same time (combined CPU + GPU load)
    u32 seconds = 600;              // per test in sequence, total when together; 0 = until stopped
    CpuKernel cpu_kernel = CK_MIXED;
    u32 core_seconds = 60;          // per-core cycling: time on each core before moving on
    u32 mem_pct = 70;               // RAM test size: % of the memory available at start (capped, see stab_mem.cpp)
    u64 mem_bytes = 0;              // explicit size; overrides mem_pct
    u32 vram_pct = 85;              // VRAM test size: % of the adapter's free local memory budget
    u64 vram_bytes = 0;             // explicit size; overrides vram_pct
    u32 gpu_adapter = 0;            // index into g_sys.gpus
    char disk_drive = 'C';
    u64 disk_bytes = 256 * MiB;     // disk verify file size
    f32 max_cpu_c = 95.0f;          // stop at these temperatures (0 = no limit)
    f32 max_gpu_c = 87.0f;
    f32 max_drive_c = 70.0f;
    bool stop_on_error = false;
    bool inject = false;            // dev self-test: each test injects one fault to prove that detection works
};

enum StabStatus : u8 { SS_PENDING, SS_RUNNING, SS_PASS, SS_FAIL, SS_STOPPED, SS_ERROR, SS_NOT_RUN };
const char* stab_status_name(StabStatus s);    // "pending", "running", "pass", "fail", "stopped", "error", "not run"

constexpr u32 STAB_MAX_LP = 64;
constexpr u32 STAB_MAX_CHUNKS = 64;

struct StabTestState {
    std::atomic<u8> status;                     // StabStatus
    std::atomic<u64> errors;
    std::atomic<f64> first_error_s;             // seconds after this test started; < 0 = none
    std::atomic<f64> work;                      // in g_stab_tests[].work_unit
    std::atomic<const char*> phase;             // current kernel / pattern (static string)
    std::atomic<u32> lp_errors[STAB_MAX_LP];    // by logical processor (CPU, cache, cores, memory)
    std::atomic<f32> lp_seconds[STAB_MAX_LP];   // per-core cycling: time each LP was under load
    std::atomic<u32> chunk_errors[STAB_MAX_CHUNKS];     // memory region / VRAM chunk / file chunk
    u32 chunk_count;
    u64 chunk_bytes;
    u64 size_bytes;                             // memory / VRAM / file actually tested
    f64 start_t, end_t;                         // timer_now()
    char note[192];                             // setup notes ("memory not locked: ..."), or why it failed to run
};

constexpr u32 STAB_MAX_LOG = 64;
struct StabError {
    u8 test;                // StabTest
    f64 t;                  // seconds after the run started
    i32 lp;                 // logical processor, -1 if not applicable
    i32 chunk;              // region / chunk, -1 if not applicable
    u64 address;            // memory: virtual address; VRAM: byte offset in the chunk; disk: byte offset in the file
    char what[40];          // kernel or pattern
    u64 expected, actual;   // the compared word (actual ^ expected = flipped bits)
};

// Live sensors, sampled once a second during a run.
enum StabSeries : u8 { SH_CPU_MHZ, SH_CPU_UTIL, SH_CPU_TEMP, SH_GPU_MHZ, SH_GPU_UTIL, SH_GPU_TEMP, SH_GPU_POWER,
                       SH_DRIVE_TEMP, SH_COUNT };
constexpr u32 STAB_HIST = 600;
struct StabSensors {
    f32 now[SH_COUNT];      // latest (NaN = not reported)
    f32 avg[SH_COUNT];      // time-weighted over the run
    f32 max[SH_COUNT];
    f32 hist[SH_COUNT][STAB_HIST];  // oldest first
    u32 hist_count;
};

struct StabRun {
    std::atomic<bool> running;
    std::atomic<bool> stop;
    StabConfig cfg;                 // snapshot taken at start
    f64 start_t, end_t;
    std::atomic<i32> current;       // test running in sequence mode (-1 together / none)
    char stop_reason[192];          // "" = ran to the end
    bool limit_stop;                // a safety limit stopped the run
    StabTestState tests[STAB_COUNT];
    u32 whea_corrected, whea_uncorrected;   // during the run
    bool whea_active;
};
extern StabRun g_stab;

// CLI ids, comma-separated ("cpu,mem", "all", "combined" = cpu + gpu together). False if an id is unknown.
bool stab_parse_ids(const char* list, StabConfig* cfg, char* err, u32 err_size);
bool stab_start(const StabConfig* cfg, char* err, u32 err_size);
void stab_stop(const char* reason);         // user stop; reason shown in the report ("stopped by user")
bool stab_is_running();
void stab_wait();
u64 stab_total_errors();
// "pass" / "fail" / "stopped" for the whole run, with a one-line explanation.
const char* stab_verdict(char* why, u32 size);
u32 stab_error_log(StabError* out, u32 max);   // first errors, oldest first
void stab_sensors(StabSensors* out);
f64 stab_elapsed();                         // seconds since the run started (until it ended)
// Report as TOON (`kind: stability`). Only after a run (not while running).
bool stab_write_toon(const wchar_t* path);
