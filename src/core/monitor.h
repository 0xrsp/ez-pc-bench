#pragma once
// Run monitor: while a run is active, a background thread samples every 250 ms
//   - CPU utilization and the clock of a busy core (measured in place, see monitor.cpp),
//   - the test GPU's core / memory clock, temperature and power (D3DKMT perf data) and engine utilization
//     (PDH "GPU Engine"),
//   - the power source and, on battery, the discharge rate (core/battery.*, read at most once a second),
// and folds the samples into per-test aggregates (g_env, indexed by bench id) that are stored with the
// results. No driver needed; values a platform doesn't report stay NaN.
#include "core/bench.h"

enum EnvFlags : u32 {
    ENV_BATTERY       = 1u << 0,    // on battery for at least one sample
    ENV_POWER_CHANGED = 1u << 1,    // AC <-> battery switch during the test
    ENV_CPU_DROP      = 1u << 2,    // CPU clock fell >= 10 % from the first to the last third of a phase
    ENV_GPU_DROP      = 1u << 3,    // same for the GPU core clock
    ENV_CPU_HOT       = 1u << 4,    // thermal zone >= 95 C
    ENV_GPU_HOT       = 1u << 5,    // GPU at or above its driver warning temperature
};

struct TestEnv {
    u32 samples;
    f32 cpu_mhz;            // clock of a busy core, average over the samples where a core was busy
    f32 cpu_mhz_max;        // highest of those samples
    f32 cpu_util;           // % of all logical processors, average
    f32 cpu_temp_max;       // C; from PawnIO (adv_sensors.h) when installed, else NaN
    f32 gpu_mhz, gpu_mhz_max;   // test GPU core clock (where the driver reports it)
    f32 gpu_mem_mhz;        // memory clock, average
    f32 gpu_util;           // % of the busiest engine, average
    f32 gpu_temp_max;       // C
    f32 gpu_power;          // % of the driver's power limit, average
    f32 cpu_drop, gpu_drop; // % clock drop, first third -> last third of the worst phase (positive = slower)
    u32 flags;              // EnvFlags
    f32 bat_discharge_w;    // W drawn from the battery, average while discharging (NaN on AC / no battery)
};

extern TestEnv g_env[MAX_BENCHES];

// Runner worker: start / stop sampling for a run (gpu_adapter = index into g_sys.gpus).
void monitor_begin(u32 gpu_adapter);
void monitor_end();
// Runner, right before a test is marked finished: publishes that test's aggregate (id < 0: whatever is pending).
void monitor_test_end(i32 id);
void monitor_reset();       // clears g_env (results_reset calls it)

bool env_valid(const TestEnv* e);
bool env_is_gpu_test(u32 bench_id);
// "CPU 4.71 GHz (max 5.05), 97 % busy, 78 °C" (+ " · GPU 2.61 GHz, 99 % busy, 71 °C, power 88 %" for GPU tests).
void env_describe(const TestEnv* e, bool gpu, char* buf, u32 size);
// Short form for a narrow column: "CPU 4.71 GHz, 97% busy" or, for GPU tests, "GPU 2.61 GHz, 71 °C".
void env_brief(const TestEnv* e, bool gpu, char* buf, u32 size);
// Warnings worth showing next to the result ("on battery, CPU clock -12 %"), "" if none.
void env_warnings(const TestEnv* e, char* buf, u32 size);
// Flag names for the results file: "battery|cpu-drop", "" if none.
void env_flag_names(u32 flags, char* buf, u32 size);
u32 env_parse_flags(const char* names);
