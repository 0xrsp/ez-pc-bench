// The whole benchmark registry: one table of suites, one table of tests.
// Order here is run order within a suite.
#include "core/bench.h"

#define BENCH_FN(name) void name(void*, const BenchParams*, BenchLive*, BenchResult*, const std::atomic<bool>*)
#define SUITE_FNS(prefix) \
    bool prefix##_init(const BenchParams*, void**, char*, u32); \
    void prefix##_shutdown(void*)

// smoke_bench.cpp
SUITE_FNS(smoke);
BENCH_FN(smoke_spin);
BENCH_FN(smoke_sweep);
BENCH_FN(smoke_matrix);

// cpu/cpu_bench.cpp
SUITE_FNS(cpu);
BENCH_FN(cpu_int_alu);
BENCH_FN(cpu_int_mul);
BENCH_FN(cpu_fma_scalar);
BENCH_FN(cpu_fma_avx2_f32);
BENCH_FN(cpu_fma_avx2_f64);
BENCH_FN(cpu_fma_avx512_f32);
BENCH_FN(cpu_fma_avx512_f64);
BENCH_FN(cpu_sgemm);
BENCH_FN(cpu_sha256);
BENCH_FN(cpu_aes);
BENCH_FN(cpu_sort);
BENCH_FN(cpu_sieve);
BENCH_FN(cpu_c2c);
BENCH_FN(cpu_scaling);

// mem/mem_bench.cpp
SUITE_FNS(mem);
BENCH_FN(mem_read);
BENCH_FN(mem_write);
BENCH_FN(mem_copy);
BENCH_FN(mem_triad);
BENCH_FN(mem_latency);
BENCH_FN(mem_sweep_st);
BENCH_FN(mem_sweep_mt);

const SuiteDesc g_suites[SUITE_COUNT] = {
    { SUITE_SMOKE,     "Smoke test",   "smoke",     smoke_init, smoke_shutdown },
    { SUITE_CPU,       "CPU",          "cpu",       cpu_init,   cpu_shutdown },
    { SUITE_MEM,       "Memory",       "mem",       mem_init,   mem_shutdown },
    { SUITE_GPU_VK,    "GPU (Vulkan)", "gpu_vk",    nullptr, nullptr },
    { SUITE_GPU_D3D12, "GPU (D3D12)",  "gpu_d3d12", nullptr, nullptr },
    { SUITE_DISK,      "Disk",         "disk",      nullptr, nullptr },
};

const BenchDesc g_benches[] = {
    { SUITE_SMOKE, "Runner", "Spin (values)",            "smoke.spin",   smoke_spin,   BENCH_DEFAULT_OFF },
    { SUITE_SMOKE, "Runner", "Memset sweep (series)",    "smoke.sweep",  smoke_sweep,  BENCH_DEFAULT_OFF },
    { SUITE_SMOKE, "Runner", "Misaligned copy (matrix)", "smoke.matrix", smoke_matrix, BENCH_DEFAULT_OFF },

    { SUITE_CPU, "Integer",        "ALU add/xor",               "cpu.int.alu",       cpu_int_alu,        0 },
    { SUITE_CPU, "Integer",        "Multiply (imul)",           "cpu.int.mul",       cpu_int_mul,        0 },
    { SUITE_CPU, "Floating point", "FP32 FMA scalar",           "cpu.fp.scalar",     cpu_fma_scalar,     0 },
    { SUITE_CPU, "Floating point", "FP32 FMA AVX2",             "cpu.fp.avx2_f32",   cpu_fma_avx2_f32,   0 },
    { SUITE_CPU, "Floating point", "FP64 FMA AVX2",             "cpu.fp.avx2_f64",   cpu_fma_avx2_f64,   0 },
    { SUITE_CPU, "Floating point", "FP32 FMA AVX-512",          "cpu.fp.avx512_f32", cpu_fma_avx512_f32, 0 },
    { SUITE_CPU, "Floating point", "FP64 FMA AVX-512",          "cpu.fp.avx512_f64", cpu_fma_avx512_f64, 0 },
    { SUITE_CPU, "Floating point", "SGEMM 1920x1920 (AVX2)",    "cpu.fp.sgemm",      cpu_sgemm,          0 },
    { SUITE_CPU, "Crypto",         "SHA-256 (SHA-NI)",          "cpu.crypto.sha256", cpu_sha256,         0 },
    { SUITE_CPU, "Crypto",         "AES-128-CTR",               "cpu.crypto.aes",    cpu_aes,            0 },
    { SUITE_CPU, "Workloads",      "Sort 16M u32",              "cpu.work.sort",     cpu_sort,           0 },
    { SUITE_CPU, "Workloads",      "Prime sieve to 1e9",        "cpu.work.sieve",    cpu_sieve,          0 },
    { SUITE_CPU, "Topology",       "Core-to-core latency",      "cpu.topo.c2c",      cpu_c2c,            0 },
    { SUITE_CPU, "Topology",       "Thread scaling (AVX2 FMA)", "cpu.topo.scaling",  cpu_scaling,        0 },

    { SUITE_MEM, "DRAM bandwidth", "Read",                      "mem.bw.read",       mem_read,           0 },
    { SUITE_MEM, "DRAM bandwidth", "Write",                     "mem.bw.write",      mem_write,          0 },
    { SUITE_MEM, "DRAM bandwidth", "Copy",                      "mem.bw.copy",       mem_copy,           0 },
    { SUITE_MEM, "DRAM bandwidth", "Triad (a = b + s*c)",       "mem.bw.triad",      mem_triad,          0 },
    { SUITE_MEM, "Latency",        "Random access latency",     "mem.lat.chase",     mem_latency,        0 },
    { SUITE_MEM, "Cache sweep",    "Read sweep, one thread",    "mem.sweep.st",      mem_sweep_st,       0 },
    { SUITE_MEM, "Cache sweep",    "Read sweep, all threads",   "mem.sweep.mt",      mem_sweep_mt,       0 },
};

const u32 g_bench_count = ARRAY_COUNT(g_benches);
static_assert(ARRAY_COUNT(g_benches) <= MAX_BENCHES, "raise MAX_BENCHES");
