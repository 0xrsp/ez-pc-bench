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
// cpu/cpu_topo.cpp, cpu_compress.cpp, cpu_image.cpp, cpu_parse.cpp, cpu_sci.cpp, cpu_hash.cpp, cpu_ai.cpp,
// cpu_pi.cpp, cpu_micro.cpp
BENCH_FN(cpu_percore);
BENCH_FN(cpu_hybrid);
BENCH_FN(cpu_comp_xpress);
BENCH_FN(cpu_comp_xpress_huff);
BENCH_FN(cpu_comp_mszip);
BENCH_FN(cpu_comp_lzms);
BENCH_FN(cpu_comp_lz77);
BENCH_FN(cpu_img_jpeg);
BENCH_FN(cpu_img_png);
BENCH_FN(cpu_img_resize);
BENCH_FN(cpu_parse_json);
BENCH_FN(cpu_parse_text);
BENCH_FN(cpu_fft);
BENCH_FN(cpu_nbody);
BENCH_FN(cpu_raybox);
BENCH_FN(cpu_raytri);
BENCH_FN(cpu_crc32c);
BENCH_FN(cpu_xxhash);
BENCH_FN(cpu_aes_gcm);
BENCH_FN(cpu_chacha20);
BENCH_FN(cpu_ai_int8);
BENCH_FN(cpu_ai_bf16);
BENCH_FN(cpu_pi);
BENCH_FN(cpu_instr_table);
BENCH_FN(cpu_branch);
BENCH_FN(cpu_code_footprint);

// mem/mem_bench.cpp
SUITE_FNS(mem);
BENCH_FN(mem_read);
BENCH_FN(mem_write);
BENCH_FN(mem_copy);
BENCH_FN(mem_triad);
BENCH_FN(mem_latency);
BENCH_FN(mem_sweep_st);
BENCH_FN(mem_sweep_mt);

// disk/disk_bench.cpp
SUITE_FNS(disk);
BENCH_FN(disk_seq1m_q8t1);
BENCH_FN(disk_seq1m_q1t1);
BENCH_FN(disk_rnd4k_q32t1);
BENCH_FN(disk_rnd4k_q1t1);
BENCH_FN(disk_seq128k_q32t1);
BENCH_FN(disk_rnd4k_q32t16);

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
    { SUITE_CPU, "Crypto",         "AES-128-GCM (Windows CNG)", "cpu.crypto.aes_gcm", cpu_aes_gcm,       0 },
    { SUITE_CPU, "Crypto",         "ChaCha20 (AVX2)",           "cpu.crypto.chacha20", cpu_chacha20,     0 },
    { SUITE_CPU, "Crypto",         "CRC32C (SSE4.2)",           "cpu.crypto.crc32c", cpu_crc32c,         0 },
    { SUITE_CPU, "Crypto",         "xxHash64",                  "cpu.crypto.xxhash", cpu_xxhash,         0 },
    { SUITE_CPU, "Workloads",      "Sort 16M u32",              "cpu.work.sort",     cpu_sort,           0 },
    { SUITE_CPU, "Workloads",      "Prime sieve to 1e9",        "cpu.work.sieve",    cpu_sieve,          0 },
    { SUITE_CPU, "Workloads",      "Pi, 1M digits (Chudnovsky, FFT)", "cpu.work.pi", cpu_pi,             BENCH_DEFAULT_OFF },
    { SUITE_CPU, "Compression",    "XPRESS (Windows)",          "cpu.comp.xpress",   cpu_comp_xpress,    0 },
    { SUITE_CPU, "Compression",    "XPRESS Huffman (Windows)",  "cpu.comp.xpress_huff", cpu_comp_xpress_huff, 0 },
    { SUITE_CPU, "Compression",    "MSZIP / Deflate (Windows)", "cpu.comp.mszip",    cpu_comp_mszip,     0 },
    { SUITE_CPU, "Compression",    "LZMS (Windows)",            "cpu.comp.lzms",     cpu_comp_lzms,      0 },
    { SUITE_CPU, "Compression",    "LZ77 (own, LZ4-style)",     "cpu.comp.lz77",     cpu_comp_lz77,      0 },
    { SUITE_CPU, "Image",          "JPEG decode (WIC)",         "cpu.img.jpeg",      cpu_img_jpeg,       0 },
    { SUITE_CPU, "Image",          "PNG decode (WIC)",          "cpu.img.png",       cpu_img_png,        0 },
    { SUITE_CPU, "Image",          "Resize 1080p to 720p, cubic (WIC)", "cpu.img.resize", cpu_img_resize, 0 },
    { SUITE_CPU, "Parsing",        "JSON parse",                "cpu.parse.json",    cpu_parse_json,     0 },
    { SUITE_CPU, "Parsing",        "Tokenize + count words",    "cpu.parse.text",    cpu_parse_text,     0 },
    { SUITE_CPU, "Science",        "FFT 16K complex (FP64)",   "cpu.sci.fft",       cpu_fft,            0 },
    { SUITE_CPU, "Science",        "N-body 2048 (FP32)",        "cpu.sci.nbody",     cpu_nbody,          0 },
    { SUITE_CPU, "Science",        "Ray-box intersection",      "cpu.sci.raybox",    cpu_raybox,         0 },
    { SUITE_CPU, "Science",        "Ray-triangle intersection", "cpu.sci.raytri",    cpu_raytri,         0 },
    { SUITE_CPU, "AI inference",   "INT8 matrix multiply",      "cpu.ai.int8",       cpu_ai_int8,        0 },
    { SUITE_CPU, "AI inference",   "BF16 matrix multiply",      "cpu.ai.bf16",       cpu_ai_bf16,        0 },
    { SUITE_CPU, "Topology",       "Core-to-core latency",      "cpu.topo.c2c",      cpu_c2c,            0 },
    { SUITE_CPU, "Topology",       "Thread scaling (AVX2 FMA)", "cpu.topo.scaling",  cpu_scaling,        0 },
    { SUITE_CPU, "Topology",       "Per-core single thread",    "cpu.topo.percore",  cpu_percore,        0 },
    { SUITE_CPU, "Topology",       "P-cores vs E-cores (hybrid)", "cpu.topo.hybrid", cpu_hybrid,         0 },
    { SUITE_CPU, "Microarchitecture", "Instruction latency / throughput", "cpu.micro.instr", cpu_instr_table, BENCH_DEFAULT_OFF },
    { SUITE_CPU, "Microarchitecture", "Branch predictor",       "cpu.micro.branch",  cpu_branch,         BENCH_DEFAULT_OFF },
    { SUITE_CPU, "Microarchitecture", "Code footprint (op cache, L1i)", "cpu.micro.code", cpu_code_footprint, BENCH_DEFAULT_OFF },

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
    { SUITE_DISK, "Random",     "RND4K Q32T1",          "disk.rnd4k_q32t1",   disk_rnd4k_q32t1,   0 },
    { SUITE_DISK, "Random",     "RND4K Q1T1",           "disk.rnd4k_q1t1",    disk_rnd4k_q1t1,    0 },
    { SUITE_DISK, "Random",     "RND4K Q32T16 (NVMe)",  "disk.rnd4k_q32t16",  disk_rnd4k_q32t16,  BENCH_DEFAULT_OFF },

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
