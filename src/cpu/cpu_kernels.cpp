// Throughput kernels in inline asm (AT&T syntax) so the compiler can't vectorize, fold or reorder them.
// Every kernel uses enough independent chains to hide instruction latency.
#include "cpu/cpu_internal.h"

u64 k_int_alu(u64 iters) {
    u64 n = iters;
    __asm__ volatile(
        "1:\n\t"
        "add $1, %%rax\n\t"  "add $1, %%rcx\n\t"  "add $1, %%rdx\n\t"
        "add $1, %%rsi\n\t"  "add $1, %%r8\n\t"   "add $1, %%r9\n\t"
        "xor $0x5a, %%r10\n\t" "xor $0x5a, %%r11\n\t" "xor $0x5a, %%r12\n\t"
        "xor $0x5a, %%r13\n\t" "xor $0x5a, %%r14\n\t" "xor $0x5a, %%r15\n\t"
        "dec %0\n\t"
        "jnz 1b\n\t"
        : "+r"(n)
        :
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc");
    return iters * 12;
}

u64 k_int_mul(u64 iters) {
    u64 n = iters;
    __asm__ volatile(
        "mov $3, %%eax\n\t" "mov $5, %%ecx\n\t" "mov $7, %%edx\n\t"
        "mov $9, %%esi\n\t" "mov $11, %%r8d\n\t" "mov $13, %%r9d\n\t"
        "1:\n\t"
        "imul $0x9E37, %%rax, %%rax\n\t" "imul $0x9E37, %%rcx, %%rcx\n\t" "imul $0x9E37, %%rdx, %%rdx\n\t"
        "imul $0x9E37, %%rsi, %%rsi\n\t" "imul $0x9E37, %%r8, %%r8\n\t"   "imul $0x9E37, %%r9, %%r9\n\t"
        "dec %0\n\t"
        "jnz 1b\n\t"
        : "+r"(n)
        :
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "cc");
    return iters * 6;
}

// 12 FMAs into accumulators 0..11 from sources 12 and 13.
#define FMA12(OP, R)                                                                                   \
    OP " %%" R "12, %%" R "13, %%" R "0\n\t" OP " %%" R "12, %%" R "13, %%" R "1\n\t"                  \
    OP " %%" R "12, %%" R "13, %%" R "2\n\t" OP " %%" R "12, %%" R "13, %%" R "3\n\t"                  \
    OP " %%" R "12, %%" R "13, %%" R "4\n\t" OP " %%" R "12, %%" R "13, %%" R "5\n\t"                  \
    OP " %%" R "12, %%" R "13, %%" R "6\n\t" OP " %%" R "12, %%" R "13, %%" R "7\n\t"                  \
    OP " %%" R "12, %%" R "13, %%" R "8\n\t" OP " %%" R "12, %%" R "13, %%" R "9\n\t"                  \
    OP " %%" R "12, %%" R "13, %%" R "10\n\t" OP " %%" R "12, %%" R "13, %%" R "11\n\t"

// Zero all 14 registers (VEX/EVEX xor zeroes the full register).
#define ZERO14(R)                                                                                      \
    "vpxor %%xmm0, %%xmm0, %%xmm0\n\t" "vpxor %%xmm1, %%xmm1, %%xmm1\n\t" "vpxor %%xmm2, %%xmm2, %%xmm2\n\t" \
    "vpxor %%xmm3, %%xmm3, %%xmm3\n\t" "vpxor %%xmm4, %%xmm4, %%xmm4\n\t" "vpxor %%xmm5, %%xmm5, %%xmm5\n\t" \
    "vpxor %%xmm6, %%xmm6, %%xmm6\n\t" "vpxor %%xmm7, %%xmm7, %%xmm7\n\t" "vpxor %%xmm8, %%xmm8, %%xmm8\n\t" \
    "vpxor %%xmm9, %%xmm9, %%xmm9\n\t" "vpxor %%xmm10, %%xmm10, %%xmm10\n\t" "vpxor %%xmm11, %%xmm11, %%xmm11\n\t" \
    "vpxor %%xmm12, %%xmm12, %%xmm12\n\t" "vpxor %%xmm13, %%xmm13, %%xmm13\n\t"

#define XMM_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", \
                     "xmm10", "xmm11", "xmm12", "xmm13", "cc"
#define ZMM_CLOBBERS "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", \
                     "zmm10", "zmm11", "zmm12", "zmm13", "cc"

u64 k_fma_ss(u64 iters) {
    u64 n = iters;
    __asm__ volatile(ZERO14(xmm) "1:\n\t" FMA12("vfmadd231ss", "xmm") "dec %0\n\tjnz 1b\n\t"
                     : "+r"(n) : : XMM_CLOBBERS);
    return iters * 12 * 2;
}

u64 k_fma_ps256(u64 iters) {
    u64 n = iters;
    __asm__ volatile(ZERO14(ymm) "1:\n\t" FMA12("vfmadd231ps", "ymm") "dec %0\n\tjnz 1b\n\tvzeroupper\n\t"
                     : "+r"(n) : : XMM_CLOBBERS);
    return iters * 12 * 8 * 2;
}

u64 k_fma_pd256(u64 iters) {
    u64 n = iters;
    __asm__ volatile(ZERO14(ymm) "1:\n\t" FMA12("vfmadd231pd", "ymm") "dec %0\n\tjnz 1b\n\tvzeroupper\n\t"
                     : "+r"(n) : : XMM_CLOBBERS);
    return iters * 12 * 4 * 2;
}

__attribute__((target("avx512f"))) u64 k_fma_ps512(u64 iters) {
    u64 n = iters;
    __asm__ volatile(ZERO14(zmm) "1:\n\t" FMA12("vfmadd231ps", "zmm") "dec %0\n\tjnz 1b\n\tvzeroupper\n\t"
                     : "+r"(n) : : ZMM_CLOBBERS);
    return iters * 12 * 16 * 2;
}

__attribute__((target("avx512f"))) u64 k_fma_pd512(u64 iters) {
    u64 n = iters;
    __asm__ volatile(ZERO14(zmm) "1:\n\t" FMA12("vfmadd231pd", "zmm") "dec %0\n\tjnz 1b\n\tvzeroupper\n\t"
                     : "+r"(n) : : ZMM_CLOBBERS);
    return iters * 12 * 8 * 2;
}
