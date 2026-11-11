// The whole benchmark registry: one table of suites, one table of tests.
// Tests of a suite must be contiguous; order here is run order.
#include "core/bench.h"

// smoke_bench.cpp
bool smoke_init(const BenchParams*, void**, char*, u32);
void smoke_shutdown(void*);
void smoke_spin(void*, const BenchParams*, BenchLive*, BenchResult*, const std::atomic<bool>*);
void smoke_sweep(void*, const BenchParams*, BenchLive*, BenchResult*, const std::atomic<bool>*);
void smoke_matrix(void*, const BenchParams*, BenchLive*, BenchResult*, const std::atomic<bool>*);

const SuiteDesc g_suites[SUITE_COUNT] = {
    { SUITE_SMOKE,     "Smoke test",   "smoke",     smoke_init, smoke_shutdown },
    { SUITE_CPU,       "CPU",          "cpu",       nullptr, nullptr },
    { SUITE_MEM,       "Memory",       "mem",       nullptr, nullptr },
    { SUITE_GPU_VK,    "GPU (Vulkan)", "gpu_vk",    nullptr, nullptr },
    { SUITE_GPU_D3D12, "GPU (D3D12)",  "gpu_d3d12", nullptr, nullptr },
    { SUITE_DISK,      "Disk",         "disk",      nullptr, nullptr },
};

const BenchDesc g_benches[] = {
    { SUITE_SMOKE, "Runner", "Spin (values)",         "smoke.spin",   smoke_spin,   0 },
    { SUITE_SMOKE, "Runner", "Memset sweep (series)", "smoke.sweep",  smoke_sweep,  0 },
    { SUITE_SMOKE, "Runner", "Misaligned copy (matrix)", "smoke.matrix", smoke_matrix, 0 },
};

const u32 g_bench_count = ARRAY_COUNT(g_benches);
static_assert(ARRAY_COUNT(g_benches) <= MAX_BENCHES, "raise MAX_BENCHES");
