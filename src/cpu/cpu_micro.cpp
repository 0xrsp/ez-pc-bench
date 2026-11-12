// Microarchitecture probes on one pinned thread, in core clock cycles. The clock comes from a chain of
// dependent single-cycle adds timed right before each measurement (k_add_chain), so results are clk even
// while the boost clock moves.
// - Instruction table (InstLatX64 / AIDA style): latency = 20 dependent copies per loop iteration,
//   reciprocal throughput = 10 independent chains x 2. The loop's dec/jnz adds one fused op per 20.
// - Branch predictor: one conditional branch following a random taken / not-taken pattern of length L,
//   repeated; the cost per iteration rises once L exceeds what the predictor can learn.
// - Code footprint: straight-line 4-byte NOPs generated at run time (1 KiB ... 2 MiB) executed in a loop;
//   instructions per clock fall as the code leaves the op cache, L1i and L2.
#include "cpu/cpu_internal.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <windows.h>
#include <string.h>

// ---- instruction table kernels -----------------------------------------------------------------------

#define GP10(P) P(rax) P(rcx) P(rdx) P(rsi) P(rdi) P(r8) P(r9) P(r10) P(r11) P(r12)
#define GP20(P) GP10(P) GP10(P)
#define GPSAME20(P) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) \
                    P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax) P(rax)
#define V10(P) P(0) P(1) P(2) P(3) P(4) P(5) P(6) P(7) P(8) P(9)
#define V20(P) V10(P) V10(P)
#define VSAME20(P) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0) P(0)

#define GP_INIT "mov $1, %%eax\n\t" "mov $1, %%ecx\n\t" "mov $1, %%edx\n\t" "mov $1, %%esi\n\t" "mov $1, %%edi\n\t" \
                "mov $1, %%r8d\n\t" "mov $1, %%r9d\n\t" "mov $1, %%r10d\n\t" "mov $1, %%r11d\n\t" "mov $1, %%r12d\n\t" \
                "mov %1, %%r15\n\t"
#define GP_CLOBBERS "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r15", "cc", "memory"

// Integer kernel: r15 = `arg` (multiplier / crc input / pointer); INIT2 runs after the common init.
#define MICRO_GP(name, ATTR, INIT2, REP, I)                                                                 \
    ATTR static u64 name(u64 iters, u64 arg) {                                                              \
        u64 n = iters;                                                                                      \
        __asm__ volatile(GP_INIT INIT2 "1:\n\t" REP(I) "dec %0\n\tjnz 1b\n\t" : "+r"(n) : "r"(arg) : GP_CLOBBERS); \
        return iters * 20;                                                                                  \
    }

#define I_ADD(r) "add $1, %%" #r "\n\t"
#define I_IMUL(r) "imul %%r15, %%" #r "\n\t"
#define I_LEA(r) "lea 1(%%" #r ",%%" #r ",2), %%" #r "\n\t"
#define I_POPCNT(r) "popcnt %%" #r ", %%" #r "\n\t"
#define I_CRC(r) "crc32q %%r15, %%" #r "\n\t"
#define I_LOADLAT(r) "mov (%%" #r "), %%" #r "\n\t"
#define I_LOADTP(r) "mov (%%r15), %%" #r "\n\t"

MICRO_GP(lat_add, , "", GPSAME20, I_ADD)
MICRO_GP(tp_add, , "", GP20, I_ADD)
MICRO_GP(lat_imul, , "", GPSAME20, I_IMUL)
MICRO_GP(tp_imul, , "", GP20, I_IMUL)
MICRO_GP(lat_lea, , "", GPSAME20, I_LEA)
MICRO_GP(tp_lea, , "", GP20, I_LEA)
MICRO_GP(lat_popcnt, __attribute__((target("popcnt"))), "", GPSAME20, I_POPCNT)
MICRO_GP(tp_popcnt, __attribute__((target("popcnt"))), "", GP20, I_POPCNT)
MICRO_GP(lat_crc, __attribute__((target("sse4.2"))), "", GPSAME20, I_CRC)
MICRO_GP(tp_crc, __attribute__((target("sse4.2"))), "", GP20, I_CRC)
MICRO_GP(lat_load, , "mov %%r15, %%rax\n\t", GPSAME20, I_LOADLAT)
MICRO_GP(tp_load, , "", GP20, I_LOADTP)

// Vector kernel: ymm15 / ymm14 = broadcast `arg` (1.0f bits), ymm13 = 0, ymm0-9 = ymm15.
#define V_INIT(R)                                                                                        \
    "vmovq %1, %%xmm15\n\t" "vpbroadcastd %%xmm15, %%ymm15\n\t" "vmovdqa %%ymm15, %%ymm14\n\t"              \
    "vpxor %%xmm13, %%xmm13, %%xmm13\n\t"                                                                 \
    "vmovdqa %%ymm15, %%ymm0\n\t" "vmovdqa %%ymm15, %%ymm1\n\t" "vmovdqa %%ymm15, %%ymm2\n\t"               \
    "vmovdqa %%ymm15, %%ymm3\n\t" "vmovdqa %%ymm15, %%ymm4\n\t" "vmovdqa %%ymm15, %%ymm5\n\t"               \
    "vmovdqa %%ymm15, %%ymm6\n\t" "vmovdqa %%ymm15, %%ymm7\n\t" "vmovdqa %%ymm15, %%ymm8\n\t"               \
    "vmovdqa %%ymm15, %%ymm9\n\t"
#define Z_INIT                                                                                            \
    "vmovq %1, %%xmm15\n\t" "vpbroadcastd %%xmm15, %%zmm15\n\t" "vmovdqa64 %%zmm15, %%zmm14\n\t"           \
    "vmovdqa64 %%zmm15, %%zmm0\n\t" "vmovdqa64 %%zmm15, %%zmm1\n\t" "vmovdqa64 %%zmm15, %%zmm2\n\t"        \
    "vmovdqa64 %%zmm15, %%zmm3\n\t" "vmovdqa64 %%zmm15, %%zmm4\n\t" "vmovdqa64 %%zmm15, %%zmm5\n\t"        \
    "vmovdqa64 %%zmm15, %%zmm6\n\t" "vmovdqa64 %%zmm15, %%zmm7\n\t" "vmovdqa64 %%zmm15, %%zmm8\n\t"        \
    "vmovdqa64 %%zmm15, %%zmm9\n\t"
// Same for legacy-SSE instructions (aesenc, pclmulqdq): xmm only, no VEX, so they run on CPUs without AVX.
#define X_INIT                                                                                            \
    "movq %1, %%xmm15\n\t" "pshufd $0, %%xmm15, %%xmm15\n\t" "movdqa %%xmm15, %%xmm14\n\t"                 \
    "pxor %%xmm13, %%xmm13\n\t"                                                                           \
    "movdqa %%xmm15, %%xmm0\n\t" "movdqa %%xmm15, %%xmm1\n\t" "movdqa %%xmm15, %%xmm2\n\t"                  \
    "movdqa %%xmm15, %%xmm3\n\t" "movdqa %%xmm15, %%xmm4\n\t" "movdqa %%xmm15, %%xmm5\n\t"                  \
    "movdqa %%xmm15, %%xmm6\n\t" "movdqa %%xmm15, %%xmm7\n\t" "movdqa %%xmm15, %%xmm8\n\t"                  \
    "movdqa %%xmm15, %%xmm9\n\t"
#define V_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm13", \
                   "xmm14", "xmm15", "cc"
#define Z_CLOBBERS "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm13", \
                   "zmm14", "zmm15", "cc"

#define MICRO_V(name, ATTR, INIT, CLOB, REP, I)                                                                   \
    ATTR static u64 name(u64 iters, u64 arg) {                                                                     \
        u64 n = iters;                                                                                             \
        __asm__ volatile(INIT "1:\n\t" REP(I) "dec %0\n\tjnz 1b\n\tvzeroupper\n\t" : "+r"(n) : "r"(arg) : CLOB);   \
        return iters * 20;                                                                                         \
    }
#define MICRO_X(name, ATTR, REP, I)                                                                                \
    ATTR static u64 name(u64 iters, u64 arg) {                                                                     \
        u64 n = iters;                                                                                             \
        __asm__ volatile(X_INIT "1:\n\t" REP(I) "dec %0\n\tjnz 1b\n\t" : "+r"(n) : "r"(arg) : V_CLOBBERS);         \
        return iters * 20;                                                                                         \
    }

#define I_VADDPS(n) "vaddps %%ymm15, %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VMULPS(n) "vmulps %%ymm15, %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VFMAPS(n) "vfmadd231ps %%ymm15, %%ymm14, %%ymm" #n "\n\t"
#define I_VDIVPS(n) "vdivps %%ymm15, %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VSQRTPS(n) "vsqrtps %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VPADDD(n) "vpaddd %%ymm15, %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VPMULLD(n) "vpmulld %%ymm15, %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VPSHUFB(n) "vpshufb %%ymm13, %%ymm" #n ", %%ymm" #n "\n\t"
#define I_VPERMPS(n) "vpermps %%ymm" #n ", %%ymm13, %%ymm" #n "\n\t"
#define I_AESENC(n) "aesenc %%xmm15, %%xmm" #n "\n\t"
#define I_PCLMUL(n) "pclmulqdq $0, %%xmm15, %%xmm" #n "\n\t"
#define I_ZADDPS(n) "vaddps %%zmm15, %%zmm" #n ", %%zmm" #n "\n\t"
#define I_ZFMAPD(n) "vfmadd231pd %%zmm15, %%zmm14, %%zmm" #n "\n\t"

#define AVX512 __attribute__((target("avx512f")))
MICRO_V(lat_vaddps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VADDPS)
MICRO_V(tp_vaddps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VADDPS)
MICRO_V(lat_vmulps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VMULPS)
MICRO_V(tp_vmulps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VMULPS)
MICRO_V(lat_vfmaps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VFMAPS)
MICRO_V(tp_vfmaps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VFMAPS)
MICRO_V(lat_vdivps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VDIVPS)
MICRO_V(tp_vdivps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VDIVPS)
MICRO_V(lat_vsqrtps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VSQRTPS)
MICRO_V(tp_vsqrtps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VSQRTPS)
MICRO_V(lat_vpaddd, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VPADDD)
MICRO_V(tp_vpaddd, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VPADDD)
MICRO_V(lat_vpmulld, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VPMULLD)
MICRO_V(tp_vpmulld, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VPMULLD)
MICRO_V(lat_vpshufb, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VPSHUFB)
MICRO_V(tp_vpshufb, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VPSHUFB)
MICRO_V(lat_vpermps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, VSAME20, I_VPERMPS)
MICRO_V(tp_vpermps, TARGET_AVX2, V_INIT(0), V_CLOBBERS, V20, I_VPERMPS)
MICRO_X(lat_aesenc, __attribute__((target("aes"))), VSAME20, I_AESENC)
MICRO_X(tp_aesenc, __attribute__((target("aes"))), V20, I_AESENC)
MICRO_X(lat_pclmul, __attribute__((target("pclmul"))), VSAME20, I_PCLMUL)
MICRO_X(tp_pclmul, __attribute__((target("pclmul"))), V20, I_PCLMUL)
MICRO_V(lat_zaddps, AVX512, Z_INIT, Z_CLOBBERS, VSAME20, I_ZADDPS)
MICRO_V(tp_zaddps, AVX512, Z_INIT, Z_CLOBBERS, V20, I_ZADDPS)
MICRO_V(lat_zfmapd, AVX512, Z_INIT, Z_CLOBBERS, VSAME20, I_ZFMAPD)
MICRO_V(tp_zfmapd, AVX512, Z_INIT, Z_CLOBBERS, V20, I_ZFMAPD)

enum Need : u8 { NEED_NONE, NEED_AVX2, NEED_AVX512, NEED_AES, NEED_PCLMUL, NEED_SSE42 };
typedef u64 (*MicroFn)(u64 iters, u64 arg);

struct InstrRow {
    const char* lat_name;
    const char* tp_name;
    MicroFn lat, tp;
    u64 arg;            // 0 = 1.0f bits for vector kernels, multiplier for imul, ...
    Need need;
};

static const u64 ONE_F32 = 0x3F800000;
static void* s_self_ptr = &s_self_ptr;     // load latency: a pointer to itself

static const InstrRow INSTRS[] = {
    { "add r64, latency", "add r64, reciprocal throughput", lat_add, tp_add, 1, NEED_NONE },
    { "lea r64 [b+i*2+d], latency", "lea r64 [b+i*2+d], reciprocal throughput", lat_lea, tp_lea, 1, NEED_NONE },
    { "imul r64, latency", "imul r64, reciprocal throughput", lat_imul, tp_imul, 3, NEED_NONE },
    { "popcnt r64, latency", "popcnt r64, reciprocal throughput", lat_popcnt, tp_popcnt, 1, NEED_NONE },
    { "crc32 r64, latency", "crc32 r64, reciprocal throughput", lat_crc, tp_crc, 3, NEED_SSE42 },
    { "load (L1 hit), latency", "load (L1 hit), reciprocal throughput", lat_load, tp_load, 0, NEED_NONE },
    { "vaddps ymm, latency", "vaddps ymm, reciprocal throughput", lat_vaddps, tp_vaddps, ONE_F32, NEED_AVX2 },
    { "vmulps ymm, latency", "vmulps ymm, reciprocal throughput", lat_vmulps, tp_vmulps, ONE_F32, NEED_AVX2 },
    { "vfmadd231ps ymm, latency", "vfmadd231ps ymm, reciprocal throughput", lat_vfmaps, tp_vfmaps, ONE_F32, NEED_AVX2 },
    { "vdivps ymm, latency", "vdivps ymm, reciprocal throughput", lat_vdivps, tp_vdivps, ONE_F32, NEED_AVX2 },
    { "vsqrtps ymm, latency", "vsqrtps ymm, reciprocal throughput", lat_vsqrtps, tp_vsqrtps, ONE_F32, NEED_AVX2 },
    { "vpaddd ymm, latency", "vpaddd ymm, reciprocal throughput", lat_vpaddd, tp_vpaddd, ONE_F32, NEED_AVX2 },
    { "vpmulld ymm, latency", "vpmulld ymm, reciprocal throughput", lat_vpmulld, tp_vpmulld, ONE_F32, NEED_AVX2 },
    { "vpshufb ymm, latency", "vpshufb ymm, reciprocal throughput", lat_vpshufb, tp_vpshufb, ONE_F32, NEED_AVX2 },
    { "vpermps ymm, latency", "vpermps ymm, reciprocal throughput", lat_vpermps, tp_vpermps, ONE_F32, NEED_AVX2 },
    { "aesenc xmm, latency", "aesenc xmm, reciprocal throughput", lat_aesenc, tp_aesenc, ONE_F32, NEED_AES },
    { "pclmulqdq xmm, latency", "pclmulqdq xmm, reciprocal throughput", lat_pclmul, tp_pclmul, ONE_F32, NEED_PCLMUL },
    { "vaddps zmm, latency", "vaddps zmm, reciprocal throughput", lat_zaddps, tp_zaddps, ONE_F32, NEED_AVX512 },
    { "vfmadd231pd zmm, latency", "vfmadd231pd zmm, reciprocal throughput", lat_zfmapd, tp_zfmapd, ONE_F32, NEED_AVX512 },
};

// Seconds for one call of fn(iters), best of 3.
static f64 time_best(MicroFn fn, u64 iters, u64 arg) {
    f64 best = 1e30;
    for (u32 r = 0; r < 3; ++r) {
        const f64 t0 = timer_now();
        fn(iters, arg);
        const f64 dt = timer_now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

static u64 add_chain_fn(u64 iters, u64) { return k_add_chain(iters); }

// Cycles per instruction of fn: iteration count calibrated to ~target seconds, clock from the add chain.
static f64 clk_per_instr(MicroFn fn, u64 arg, f64 target) {
    u64 iters = 1024;
    f64 t = time_best(fn, iters, arg);
    if (t < target) iters = (u64)(iters * target / (t > 1e-7 ? t : 1e-7)) + 1;
    t = time_best(fn, iters, arg);
    const f64 ns_per_instr = t / (iters * 20.0);
    static constexpr u64 REF_ITERS = 1u << 17;   // ~2M cycles
    const f64 cycle = time_best(add_chain_fn, REF_ITERS, 0) / (REF_ITERS * 16.0);
    return ns_per_instr / cycle;
}

struct InstrJob {
    f64 lat[ARRAY_COUNT(INSTRS)], tp[ARRAY_COUNT(INSTRS)];
    bool have[ARRAY_COUNT(INSTRS)];
    f64 target;
    BenchLive* live;
    const CancelFlag* cancel;
};

static bool instr_supported(Need n) {
    switch (n) {
        case NEED_AVX2: return g_sys.cpu.avx2 && g_sys.cpu.fma;
        case NEED_AVX512: return g_sys.cpu.avx512f;
        case NEED_AES: return g_sys.cpu.aes;
        case NEED_PCLMUL: return g_cpu_ext.pclmul;
        case NEED_SSE42: return g_sys.cpu.sse42;
        default: return true;
    }
}

static void instr_task(u32, u32, void* user) {
    InstrJob* j = (InstrJob*)user;
    for (u32 i = 0; i < ARRAY_COUNT(INSTRS) && !j->cancel->load(); ++i) {
        const InstrRow* r = &INSTRS[i];
        j->have[i] = instr_supported(r->need);
        if (!j->have[i]) continue;
        const u64 arg = r->lat == lat_load ? (u64)&s_self_ptr : r->arg;
        j->lat[i] = clk_per_instr(r->lat, arg, j->target);
        j->tp[i] = clk_per_instr(r->tp, arg, j->target);
        live_sample(j->live, j->lat[i]);
        live_progress(j->live, (f32)(i + 1) / ARRAY_COUNT(INSTRS));
    }
}

void cpu_instr_table(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Instruction latency / throughput (one core)", "clk");
    static InstrJob j;
    memset(&j, 0, sizeof(j));
    j.target = p->duration_ms / 1000.0 / (ARRAY_COUNT(INSTRS) * 2 * 3 * 2);    // ~duration in total
    if (j.target < 0.002) j.target = 0.002;
    j.live = live;
    j.cancel = cancel;
    pool_run(1, instr_task, &j);
    res_begin(out, RESULT_VALUES);
    for (u32 i = 0; i < ARRAY_COUNT(INSTRS); ++i) {
        if (!j.have[i]) continue;
        res_add(out, INSTRS[i].lat_name, "clk", j.lat[i]);
        res_add(out, INSTRS[i].tp_name, "clk", j.tp[i]);
    }
    // Sanity: the add chain defines 1 clk, so add latency must come out as ~1.
    if (!cancel->load() && (j.lat[0] < 0.9 || j.lat[0] > 1.15)) res_fail(out, "add latency %.2f clk: clock reference unstable", j.lat[0]);
}

// ---- branch predictor --------------------------------------------------------------------------------

// `iters` iterations over a byte pattern of length `len` (repeated): one conditional branch on the byte,
// branch-free wrap-around. Returns the number of taken branches.
static u64 branch_loop(const u8* pattern, u64 len, u64 iters) {
    // The wrap value lives in r11 (zeroed here): as a separate "r"(0) input, clang gave it the same register
    // as idx, which also starts at 0, and the wrap never happened.
    u64 taken = 0, idx = 0;
    __asm__ volatile(
        "xor %%r11d, %%r11d\n\t"
        "1:\n\t"
        "movzbl (%[p],%[i]), %%eax\n\t"
        "test %%eax, %%eax\n\t"
        "jz 2f\n\t"
        "add $1, %[t]\n\t"
        "2:\n\t"
        "add $1, %[i]\n\t"
        "cmp %[len], %[i]\n\t"
        "cmove %%r11, %[i]\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        : [t] "+r"(taken), [i] "+r"(idx), [n] "+r"(iters)
        : [p] "r"(pattern), [len] "r"(len)
        : "rax", "r11", "cc", "memory");
    return taken;
}

struct BranchJob {
    u8* pattern;
    f64 clk[17];
    u32 lens[17], count;
    BenchLive* live;
    const CancelFlag* cancel;
};

static void branch_task(u32, u32, void* user) {
    BranchJob* j = (BranchJob*)user;
    static constexpr u64 ITERS = 1u << 21;
    for (u32 k = 0; k < j->count && !j->cancel->load(); ++k) {
        u64 s = 0x9E3779B97F4A7C15ull + k, ones = 0;
        for (u32 i = 0; i < j->lens[k]; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; j->pattern[i] = (u8)(s >> 63); ones += j->pattern[i]; }
        branch_loop(j->pattern, j->lens[k], ITERS);     // train
        f64 best = 1e30;
        for (u32 r = 0; r < 3; ++r) {
            const f64 t0 = timer_now();
            const u64 taken = branch_loop(j->pattern, j->lens[k], ITERS);
            const f64 dt = timer_now() - t0;
            if (dt < best) best = dt;
            if (taken != ones * (ITERS / j->lens[k])) best = -1e30;     // ITERS is a multiple of every length
        }
        const f64 cycle = time_best(add_chain_fn, 1u << 17, 0) / ((1u << 17) * 16.0);
        j->clk[k] = best < 0.0 ? -1.0 : best / ITERS / cycle;
        live_sample(j->live, j->clk[k]);
        live_progress(j->live, (f32)(k + 1) / j->count);
    }
}

void cpu_branch(void*, const BenchParams*, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Branch pattern length sweep (one core)", "clk");
    static BranchJob j;
    static u8 pattern[1u << 16];
    j.pattern = pattern;
    j.count = 0;
    for (u32 len = 2; len <= (1u << 16); len *= 2) j.lens[j.count++] = len;
    j.live = live;
    j.cancel = cancel;
    pool_run(1, branch_task, &j);
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Pattern length", "branches");
    bool ok = true;
    for (u32 k = 0; k < j.count; ++k) {
        res_add(out, "Random pattern, cost per iteration", "clk", j.clk[k], (f64)j.lens[k]);
        ok &= j.clk[k] > 0.0;
    }
    if (!ok && !cancel->load()) res_fail(out, "taken-branch count mismatch");
}

// ---- code footprint (op cache / L1i / L2) --------------------------------------------------------------

static constexpr u64 JIT_MAX = 2 * MiB;

typedef void (*JitLoop)(u64 reps);

// Writes `bytes` of 4-byte NOPs followed by `dec rcx; jnz start; ret`.
static void jit_emit(u8* code, u64 bytes) {
    for (u64 i = 0; i < bytes; i += 4) { code[i] = 0x0F; code[i + 1] = 0x1F; code[i + 2] = 0x40; code[i + 3] = 0x00; }
    u8* p = code + bytes;
    *p++ = 0x48; *p++ = 0xFF; *p++ = 0xC9;                      // dec rcx
    const i32 rel = (i32)(code - (p + 6));
    *p++ = 0x0F; *p++ = 0x85; memcpy(p, &rel, 4); p += 4;      // jnz code
    *p++ = 0xC3;                                                // ret
}

struct CodeJob {
    u8* code;
    f64 ipc[16];
    u64 sizes[16];
    u32 count;
    bool failed;
    BenchLive* live;
    const CancelFlag* cancel;
};

static void code_task(u32, u32, void* user) {
    CodeJob* j = (CodeJob*)user;
    for (u32 k = 0; k < j->count && !j->cancel->load(); ++k) {
        DWORD old;
        if (!VirtualProtect(j->code, JIT_MAX + 4096, PAGE_READWRITE, &old)) { j->failed = true; return; }
        jit_emit(j->code, j->sizes[k]);
        if (!VirtualProtect(j->code, JIT_MAX + 4096, PAGE_EXECUTE_READ, &old)) { j->failed = true; return; }
        FlushInstructionCache(GetCurrentProcess(), j->code, JIT_MAX + 4096);
        const JitLoop fn = (JitLoop)(void*)j->code;
        const u64 instrs_per_rep = j->sizes[k] / 4 + 1;
        const u64 reps = (32ull << 20) / instrs_per_rep + 1;     // ~32M instructions
        fn(reps / 8 + 1);                                       // warm up
        f64 best = 1e30;
        for (u32 r = 0; r < 3; ++r) {
            const f64 t0 = timer_now();
            fn(reps);
            const f64 dt = timer_now() - t0;
            if (dt < best) best = dt;
        }
        const f64 cycle = time_best(add_chain_fn, 1u << 17, 0) / ((1u << 17) * 16.0);
        j->ipc[k] = (f64)instrs_per_rep * reps / (best / cycle);
        live_sample(j->live, j->ipc[k]);
        live_progress(j->live, (f32)(k + 1) / j->count);
    }
}

void cpu_code_footprint(void*, const BenchParams*, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Code footprint sweep, 4-byte NOPs (one core)", "IPC");
    static CodeJob j;
    j.code = (u8*)VirtualAlloc(nullptr, JIT_MAX + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!j.code) { res_fail(out, "out of memory"); return; }
    j.count = 0;
    for (u64 s = 1 * KiB; s <= JIT_MAX; s *= 2) j.sizes[j.count++] = s;
    j.failed = false;
    j.live = live;
    j.cancel = cancel;
    pool_run(1, code_task, &j);
    VirtualFree(j.code, 0, MEM_RELEASE);
    if (j.failed) { res_fail(out, "could not make the generated code executable"); return; }
    res_begin(out, RESULT_SERIES);
    res_series_axis(out, "Code size", "bytes");
    for (u32 k = 0; k < j.count; ++k) res_add(out, "4-byte NOPs", "IPC", j.ipc[k], (f64)j.sizes[k]);
}
