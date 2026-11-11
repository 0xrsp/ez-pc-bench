// The whole benchmark registry: one table of suites, one table of tests.
// Order here is run order within a suite.
#include "core/bench.h"
#include "gpu/gpu_common.h"

#define BENCH_FN(name) void name(void*, const BenchParams*, BenchLive*, BenchResult*, const std::atomic<bool>*)
#define SUITE_FNS(prefix) \
    bool prefix##_init(const BenchParams*, void**, char*, u32); \
    void prefix##_shutdown(void*)

// smoke_bench.cpp
SUITE_FNS(smoke);
BENCH_FN(smoke_spin);
BENCH_FN(smoke_sweep);
BENCH_FN(smoke_matrix);
BENCH_FN(smoke_preview);

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

// disk/*.cpp
SUITE_FNS(disk);
BENCH_FN(disk_seq1m_q8t1);
BENCH_FN(disk_seq1m_q1t1);
BENCH_FN(disk_rnd4k_q32t1);
BENCH_FN(disk_rnd4k_q1t1);
BENCH_FN(disk_seq128k_q32t1);
BENCH_FN(disk_rnd4k_q32t16);
BENCH_FN(disk_custom1);
BENCH_FN(disk_custom2);
BENCH_FN(disk_custom3);
BENCH_FN(disk_custom4);
BENCH_FN(disk_lat_read);
BENCH_FN(disk_lat_write);
BENCH_FN(disk_lat_flush);
BENCH_FN(disk_lat_hist);
BENCH_FN(disk_lat_load);
BENCH_FN(disk_lat_wake);
BENCH_FN(disk_sweep_qd);
BENCH_FN(disk_sweep_block);
BENCH_FN(disk_sweep_compress);
BENCH_FN(disk_fs_meta);
BENCH_FN(disk_files_copy);
BENCH_FN(disk_recovery);
BENCH_FN(disk_sustained_write);
BENCH_FN(disk_raw_span);
BENCH_FN(disk_raw_scan);
BENCH_FN(disk_raw_seek);

// gpu/gpu_entry.cpp: gpu_vk_<id> and gpu_d3d12_<id> for every test in GPU_TESTS (gpu_common.h)
#define X(id, e, g, gid, name, flags) BENCH_FN(gpu_vk_##id); BENCH_FN(gpu_d3d12_##id);
GPU_TESTS(X)
#undef X

const SuiteDesc g_suites[SUITE_COUNT] = {
    { SUITE_SMOKE,     "Smoke test",   "smoke",     smoke_init, smoke_shutdown },
    { SUITE_CPU,       "CPU",          "cpu",       cpu_init,   cpu_shutdown },
    { SUITE_MEM,       "Memory",       "mem",       mem_init,   mem_shutdown },
    { SUITE_GPU_VK,    "GPU (Vulkan)", "gpu_vk",    gpu_vk_init,    gpu_vk_shutdown },
    { SUITE_GPU_D3D12, "GPU (D3D12)",  "gpu_d3d12", gpu_d3d12_init, gpu_d3d12_shutdown },
    { SUITE_DISK,      "Disk",         "disk",      disk_init,  disk_shutdown },
};

const BenchDesc g_benches[] = {
    { SUITE_SMOKE, "Runner", "Spin (values)",            "smoke.spin",   smoke_spin,   BENCH_DEFAULT_OFF },
    { SUITE_SMOKE, "Runner", "Memset sweep (series)",    "smoke.sweep",  smoke_sweep,  BENCH_DEFAULT_OFF },
    { SUITE_SMOKE, "Runner", "Misaligned copy (matrix)", "smoke.matrix", smoke_matrix, BENCH_DEFAULT_OFF },
    { SUITE_SMOKE, "Runner", "Preview (synthetic)",      "smoke.preview", smoke_preview, BENCH_DEFAULT_OFF | BENCH_PREVIEW },

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

    { SUITE_MEM, "DRAM bandwidth", "Read",                     "mem.bw.read",       mem_read,           0 },
    { SUITE_MEM, "DRAM bandwidth", "Write",                     "mem.bw.write",      mem_write,          0 },
    { SUITE_MEM, "DRAM bandwidth", "Copy",                      "mem.bw.copy",       mem_copy,           0 },
    { SUITE_MEM, "DRAM bandwidth", "Triad (a = b + s*c)",       "mem.bw.triad",      mem_triad,          0 },
    { SUITE_MEM, "Latency",        "Random access latency",     "mem.lat.chase",     mem_latency,        0 },
    { SUITE_MEM, "Cache sweep",    "Read sweep, one thread",    "mem.sweep.st",      mem_sweep_st,       0 },
    { SUITE_MEM, "Cache sweep",    "Read sweep, all threads",   "mem.sweep.mt",      mem_sweep_mt,       0 },

    // CDM 8 Default profile + NVMe profile extras (off by default). All sequential tests run before the
    // random ones: random 4K writes fragment the SSD's mapping of the test file and slow later sequential reads.
    { SUITE_DISK, "Sequential", "SEQ1M Q8T1",           "disk.seq1m_q8t1",    disk_seq1m_q8t1,    0 },
    { SUITE_DISK, "Sequential", "SEQ1M Q1T1",           "disk.seq1m_q1t1",    disk_seq1m_q1t1,    0 },
    { SUITE_DISK, "Sequential", "SEQ128K Q32T1 (NVMe)", "disk.seq128k_q32t1", disk_seq128k_q32t1, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Sequential sweeps", "Block size sweep 512 B - 64 MiB (Q4)", "disk.sweep.block", disk_sweep_block, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Sequential sweeps", "Compressibility sweep 0-100 %", "disk.sweep.compress", disk_sweep_compress, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Random",     "RND4K Q32T1",          "disk.rnd4k_q32t1",   disk_rnd4k_q32t1,   0 },
    { SUITE_DISK, "Random",     "RND4K Q1T1",           "disk.rnd4k_q1t1",    disk_rnd4k_q1t1,    0 },
    { SUITE_DISK, "Random",     "RND4K Q32T16 (NVMe)",  "disk.rnd4k_q32t16",  disk_rnd4k_q32t16,  BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Random",     "Queue depth sweep QD1-256", "disk.sweep.qd", disk_sweep_qd,   BENCH_DEFAULT_OFF },
    // User-defined rows (BenchParams::disk.custom); skipped while empty.
    { SUITE_DISK, "Custom", "Custom row 1", "disk.custom1", disk_custom1, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Custom", "Custom row 2", "disk.custom2", disk_custom2, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Custom", "Custom row 3", "disk.custom3", disk_custom3, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Custom", "Custom row 4", "disk.custom4", disk_custom4, BENCH_DEFAULT_OFF },
    // Latency (percentiles from per-I/O histograms), file system, sustained and raw-device tests.
    { SUITE_DISK, "Latency", "4K random read QD1",             "disk.lat.read",  disk_lat_read,  0 },
    { SUITE_DISK, "Latency", "4K random write QD1",            "disk.lat.write", disk_lat_write, 0 },
    { SUITE_DISK, "Latency", "4K write + flush / FUA QD1",     "disk.lat.flush", disk_lat_flush, 0 },
    { SUITE_DISK, "Latency", "Latency distribution (4K QD1)",  "disk.lat.hist",  disk_lat_hist,  BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Latency", "Read latency under write load",  "disk.lat.load",  disk_lat_load,  BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Latency", "Wake-up latency (idle 1/5/30 s)", "disk.lat.wake", disk_lat_wake,  BENCH_DEFAULT_OFF },
    { SUITE_DISK, "File system", "Metadata: create / open / stat / delete", "disk.fs.meta", disk_fs_meta, 0 },
    { SUITE_DISK, "File system", "Copy ISO / program / game sets", "disk.files.copy", disk_files_copy, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Sustained", "TRIM / idle recovery",        "disk.recovery",        disk_recovery,        BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Sustained", "Sustained write (SLC cache)", "disk.sustained.write", disk_sustained_write, BENCH_DEFAULT_OFF | BENCH_CONFIRM },
    { SUITE_DISK, "Raw device (admin, read-only)", "Full-span read curve", "disk.raw.span", disk_raw_span, BENCH_DEFAULT_OFF },
    { SUITE_DISK, "Raw device (admin, read-only)", "Surface scan",         "disk.raw.scan", disk_raw_scan, BENCH_DEFAULT_OFF | BENCH_CONFIRM },
    { SUITE_DISK, "Raw device (admin, read-only)", "Seek profile",         "disk.raw.seek", disk_raw_seek, BENCH_DEFAULT_OFF },

    // GPU: same tests for both APIs, ids like "gpu_vk.compute.fp32", "gpu_d3d12.3d.scene".
#define X(id, e, g, gid, name, flags) { SUITE_GPU_VK, g, name, "gpu_vk." gid "." #id, gpu_vk_##id, flags },
    GPU_TESTS(X)
#undef X
#define X(id, e, g, gid, name, flags) { SUITE_GPU_D3D12, g, name, "gpu_d3d12." gid "." #id, gpu_d3d12_##id, flags },
    GPU_TESTS(X)
#undef X
};

const u32 g_bench_count = ARRAY_COUNT(g_benches);
static_assert(ARRAY_COUNT(g_benches) <= MAX_BENCHES, "raise MAX_BENCHES");
