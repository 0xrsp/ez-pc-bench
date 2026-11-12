// Memory kernels in inline asm (AT&T syntax): 256 bytes per loop iteration in 8 ymm registers (AVX2) or, on
// CPUs without AVX2, 16 xmm registers (SSE2). The exported k_mem_* pick one.
#include "mem/mem_kernels.h"
#include "core/sysinfo.h"

#define YMM_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory"

TARGET_AVX2 static void k_mem_read_avx2(const void* p, u64 bytes) {
    const u8* ptr = (const u8*)p;
    const u8* end = ptr + bytes;
    __asm__ volatile(
        "1:\n\t"
        "vmovdqa 0(%0), %%ymm0\n\t"   "vmovdqa 32(%0), %%ymm1\n\t"
        "vmovdqa 64(%0), %%ymm2\n\t"  "vmovdqa 96(%0), %%ymm3\n\t"
        "vmovdqa 128(%0), %%ymm4\n\t" "vmovdqa 160(%0), %%ymm5\n\t"
        "vmovdqa 192(%0), %%ymm6\n\t" "vmovdqa 224(%0), %%ymm7\n\t"
        "add $256, %0\n\t"
        "cmp %1, %0\n\t"
        "jb 1b\n\t"
        "vzeroupper\n\t"
        : "+r"(ptr) : "r"(end) : YMM_CLOBBERS);
}

TARGET_AVX2 static void k_mem_write_avx2(void* p, u64 bytes) {
    u8* ptr = (u8*)p;
    const u8* end = ptr + bytes;
    __asm__ volatile(
        "vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\t"      // all ones, non-zero data
        "1:\n\t"
        "vmovdqa %%ymm0, 0(%0)\n\t"   "vmovdqa %%ymm0, 32(%0)\n\t"
        "vmovdqa %%ymm0, 64(%0)\n\t"  "vmovdqa %%ymm0, 96(%0)\n\t"
        "vmovdqa %%ymm0, 128(%0)\n\t" "vmovdqa %%ymm0, 160(%0)\n\t"
        "vmovdqa %%ymm0, 192(%0)\n\t" "vmovdqa %%ymm0, 224(%0)\n\t"
        "add $256, %0\n\t"
        "cmp %1, %0\n\t"
        "jb 1b\n\t"
        "vzeroupper\n\t"
        : "+r"(ptr) : "r"(end) : YMM_CLOBBERS);
}

TARGET_AVX2 static void k_mem_write_nt_avx2(void* p, u64 bytes) {
    u8* ptr = (u8*)p;
    const u8* end = ptr + bytes;
    __asm__ volatile(
        "vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\t"
        "1:\n\t"
        "vmovntdq %%ymm0, 0(%0)\n\t"   "vmovntdq %%ymm0, 32(%0)\n\t"
        "vmovntdq %%ymm0, 64(%0)\n\t"  "vmovntdq %%ymm0, 96(%0)\n\t"
        "vmovntdq %%ymm0, 128(%0)\n\t" "vmovntdq %%ymm0, 160(%0)\n\t"
        "vmovntdq %%ymm0, 192(%0)\n\t" "vmovntdq %%ymm0, 224(%0)\n\t"
        "add $256, %0\n\t"
        "cmp %1, %0\n\t"
        "jb 1b\n\t"
        "sfence\n\t"
        "vzeroupper\n\t"
        : "+r"(ptr) : "r"(end) : YMM_CLOBBERS);
}

#define COPY_LOADS                                                               \
    "vmovdqa 0(%1), %%ymm0\n\t"   "vmovdqa 32(%1), %%ymm1\n\t"                   \
    "vmovdqa 64(%1), %%ymm2\n\t"  "vmovdqa 96(%1), %%ymm3\n\t"                   \
    "vmovdqa 128(%1), %%ymm4\n\t" "vmovdqa 160(%1), %%ymm5\n\t"                  \
    "vmovdqa 192(%1), %%ymm6\n\t" "vmovdqa 224(%1), %%ymm7\n\t"

#define COPY_STORES(OP)                                                          \
    OP " %%ymm0, 0(%0)\n\t"   OP " %%ymm1, 32(%0)\n\t"                           \
    OP " %%ymm2, 64(%0)\n\t"  OP " %%ymm3, 96(%0)\n\t"                           \
    OP " %%ymm4, 128(%0)\n\t" OP " %%ymm5, 160(%0)\n\t"                          \
    OP " %%ymm6, 192(%0)\n\t" OP " %%ymm7, 224(%0)\n\t"

TARGET_AVX2 static void k_mem_copy_avx2(void* dst, const void* src, u64 bytes) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    const u8* end = s + bytes;
    __asm__ volatile(
        "1:\n\t" COPY_LOADS COPY_STORES("vmovdqa")
        "add $256, %0\n\t"
        "add $256, %1\n\t"
        "cmp %2, %1\n\t"
        "jb 1b\n\t"
        "vzeroupper\n\t"
        : "+r"(d), "+r"(s) : "r"(end) : YMM_CLOBBERS);
}

TARGET_AVX2 static void k_mem_copy_nt_avx2(void* dst, const void* src, u64 bytes) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    const u8* end = s + bytes;
    __asm__ volatile(
        "1:\n\t" COPY_LOADS COPY_STORES("vmovntdq")
        "add $256, %0\n\t"
        "add $256, %1\n\t"
        "cmp %2, %1\n\t"
        "jb 1b\n\t"
        "sfence\n\t"
        "vzeroupper\n\t"
        : "+r"(d), "+r"(s) : "r"(end) : YMM_CLOBBERS);
}

// STREAM triad, 128 bytes per array per iteration.
TARGET_AVX2 static void k_mem_triad_avx2(f32* a, const f32* b, const f32* c, f32 s, u64 bytes_per_array) {
    u64 i = 0;
    __asm__ volatile(
        "vbroadcastss %[s], %%ymm7\n\t"
        "1:\n\t"
        "vmovaps 0(%[b],%[i]), %%ymm0\n\t"  "vmovaps 32(%[b],%[i]), %%ymm1\n\t"
        "vmovaps 64(%[b],%[i]), %%ymm2\n\t" "vmovaps 96(%[b],%[i]), %%ymm3\n\t"
        "vfmadd231ps 0(%[c],%[i]), %%ymm7, %%ymm0\n\t"  "vfmadd231ps 32(%[c],%[i]), %%ymm7, %%ymm1\n\t"
        "vfmadd231ps 64(%[c],%[i]), %%ymm7, %%ymm2\n\t" "vfmadd231ps 96(%[c],%[i]), %%ymm7, %%ymm3\n\t"
        "vmovaps %%ymm0, 0(%[a],%[i])\n\t"  "vmovaps %%ymm1, 32(%[a],%[i])\n\t"
        "vmovaps %%ymm2, 64(%[a],%[i])\n\t" "vmovaps %%ymm3, 96(%[a],%[i])\n\t"
        "add $128, %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jb 1b\n\t"
        "vzeroupper\n\t"
        : [i] "+r"(i)
        : [a] "r"(a), [b] "r"(b), [c] "r"(c), [n] "r"(bytes_per_array), [s] "m"(s)
        : YMM_CLOBBERS);
}

void* k_mem_chase(void* p, u64 steps) {
    __asm__ volatile(
        "1:\n\t"
        "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t"
        "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t"
        "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t"
        "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t" "mov (%0), %0\n\t"
        "sub $16, %1\n\t"
        "jnz 1b\n\t"
        : "+r"(p), "+r"(steps) : : "cc", "memory");
    return p;
}

TARGET_AVX2 static void k_mem_read_delay_avx2(const void* p, u64 bytes, u64 delay) {
    const u8* ptr = (const u8*)p;
    const u8* end = ptr + bytes;
    u64 spin;
    __asm__ volatile(
        "1:\n\t"
        "vmovdqa 0(%0), %%ymm0\n\t"   "vmovdqa 32(%0), %%ymm1\n\t"
        "vmovdqa 64(%0), %%ymm2\n\t"  "vmovdqa 96(%0), %%ymm3\n\t"
        "vmovdqa 128(%0), %%ymm4\n\t" "vmovdqa 160(%0), %%ymm5\n\t"
        "vmovdqa 192(%0), %%ymm6\n\t" "vmovdqa 224(%0), %%ymm7\n\t"
        "mov %3, %1\n\t"
        "test %1, %1\n\t"
        "jz 3f\n\t"
        "2:\n\t"
        "dec %1\n\t"
        "jnz 2b\n\t"
        "3:\n\t"
        "add $256, %0\n\t"
        "cmp %2, %0\n\t"
        "jb 1b\n\t"
        "vzeroupper\n\t"
        : "+r"(ptr), "=&r"(spin) : "r"(end), "r"(delay) : YMM_CLOBBERS);
}

#define MIX_LOADS                                                                \
    "vmovdqa 0(%1), %%ymm0\n\t"  "vmovdqa 32(%1), %%ymm1\n\t"                    \
    "vmovdqa 64(%1), %%ymm2\n\t" "vmovdqa 96(%1), %%ymm3\n\t"
#define MIX_XOR(O0, O1, O2, O3)                                                  \
    "vpxor " O0 "(%1), %%ymm0, %%ymm0\n\t" "vpxor " O1 "(%1), %%ymm1, %%ymm1\n\t" \
    "vpxor " O2 "(%1), %%ymm2, %%ymm2\n\t" "vpxor " O3 "(%1), %%ymm3, %%ymm3\n\t"
#define MIX_STORES                                                               \
    "vmovdqa %%ymm0, 0(%0)\n\t"  "vmovdqa %%ymm1, 32(%0)\n\t"                    \
    "vmovdqa %%ymm2, 64(%0)\n\t" "vmovdqa %%ymm3, 96(%0)\n\t"

TARGET_AVX2 static void k_mem_mix_avx2(void* dst, const void* src, u64 dst_bytes, u32 reads) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    const u8* end = d + dst_bytes;
    if (reads <= 1) {
        __asm__ volatile(
            "1:\n\t" MIX_LOADS MIX_STORES
            "add $128, %0\n\t" "add $128, %1\n\t"
            "cmp %2, %0\n\t" "jb 1b\n\t" "vzeroupper\n\t"
            : "+r"(d), "+r"(s) : "r"(end) : YMM_CLOBBERS);
    } else if (reads == 2) {
        __asm__ volatile(
            "1:\n\t" MIX_LOADS MIX_XOR("128", "160", "192", "224") MIX_STORES
            "add $128, %0\n\t" "add $256, %1\n\t"
            "cmp %2, %0\n\t" "jb 1b\n\t" "vzeroupper\n\t"
            : "+r"(d), "+r"(s) : "r"(end) : YMM_CLOBBERS);
    } else {
        __asm__ volatile(
            "1:\n\t" MIX_LOADS MIX_XOR("128", "160", "192", "224") MIX_XOR("256", "288", "320", "352") MIX_STORES
            "add $128, %0\n\t" "add $384, %1\n\t"
            "cmp %2, %0\n\t" "jb 1b\n\t" "vzeroupper\n\t"
            : "+r"(d), "+r"(s) : "r"(end) : YMM_CLOBBERS);
    }
}

// ---- SSE2 (CPUs without AVX2): same loops, 16 xmm registers per 256 bytes ----------------------------------

#define XMM16_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", \
                       "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc", "memory"

#define SSE_LOAD16(R)                                                                                          \
    "movdqa 0(" R "), %%xmm0\n\t"    "movdqa 16(" R "), %%xmm1\n\t"   "movdqa 32(" R "), %%xmm2\n\t"         \
    "movdqa 48(" R "), %%xmm3\n\t"   "movdqa 64(" R "), %%xmm4\n\t"   "movdqa 80(" R "), %%xmm5\n\t"         \
    "movdqa 96(" R "), %%xmm6\n\t"   "movdqa 112(" R "), %%xmm7\n\t"  "movdqa 128(" R "), %%xmm8\n\t"        \
    "movdqa 144(" R "), %%xmm9\n\t"  "movdqa 160(" R "), %%xmm10\n\t" "movdqa 176(" R "), %%xmm11\n\t"       \
    "movdqa 192(" R "), %%xmm12\n\t" "movdqa 208(" R "), %%xmm13\n\t" "movdqa 224(" R "), %%xmm14\n\t"       \
    "movdqa 240(" R "), %%xmm15\n\t"

#define SSE_STORE16(OP, R)                                                                                     \
    OP " %%xmm0, 0(" R ")\n\t"    OP " %%xmm1, 16(" R ")\n\t"   OP " %%xmm2, 32(" R ")\n\t"                  \
    OP " %%xmm3, 48(" R ")\n\t"   OP " %%xmm4, 64(" R ")\n\t"   OP " %%xmm5, 80(" R ")\n\t"                  \
    OP " %%xmm6, 96(" R ")\n\t"   OP " %%xmm7, 112(" R ")\n\t"  OP " %%xmm8, 128(" R ")\n\t"                 \
    OP " %%xmm9, 144(" R ")\n\t"  OP " %%xmm10, 160(" R ")\n\t" OP " %%xmm11, 176(" R ")\n\t"                \
    OP " %%xmm12, 192(" R ")\n\t" OP " %%xmm13, 208(" R ")\n\t" OP " %%xmm14, 224(" R ")\n\t"                \
    OP " %%xmm15, 240(" R ")\n\t"

#define SSE_FILL16(OP, R)                                                                                      \
    OP " %%xmm0, 0(" R ")\n\t"   OP " %%xmm0, 16(" R ")\n\t"  OP " %%xmm0, 32(" R ")\n\t"                    \
    OP " %%xmm0, 48(" R ")\n\t"  OP " %%xmm0, 64(" R ")\n\t"  OP " %%xmm0, 80(" R ")\n\t"                    \
    OP " %%xmm0, 96(" R ")\n\t"  OP " %%xmm0, 112(" R ")\n\t" OP " %%xmm0, 128(" R ")\n\t"                   \
    OP " %%xmm0, 144(" R ")\n\t" OP " %%xmm0, 160(" R ")\n\t" OP " %%xmm0, 176(" R ")\n\t"                   \
    OP " %%xmm0, 192(" R ")\n\t" OP " %%xmm0, 208(" R ")\n\t" OP " %%xmm0, 224(" R ")\n\t"                   \
    OP " %%xmm0, 240(" R ")\n\t"

static void k_mem_read_sse(const void* p, u64 bytes) {
    const u8* ptr = (const u8*)p;
    const u8* end = ptr + bytes;
    __asm__ volatile("1:\n\t" SSE_LOAD16("%0") "add $256, %0\n\t" "cmp %1, %0\n\t" "jb 1b\n\t"
                     : "+r"(ptr) : "r"(end) : XMM16_CLOBBERS);
}

static void k_mem_write_sse(void* p, u64 bytes, bool nt) {
    u8* ptr = (u8*)p;
    const u8* end = ptr + bytes;
    if (nt)
        __asm__ volatile("pcmpeqd %%xmm0, %%xmm0\n\t" "1:\n\t" SSE_FILL16("movntdq", "%0")
                         "add $256, %0\n\t" "cmp %1, %0\n\t" "jb 1b\n\t" "sfence\n\t"
                         : "+r"(ptr) : "r"(end) : XMM16_CLOBBERS);
    else
        __asm__ volatile("pcmpeqd %%xmm0, %%xmm0\n\t" "1:\n\t" SSE_FILL16("movdqa", "%0")
                         "add $256, %0\n\t" "cmp %1, %0\n\t" "jb 1b\n\t"
                         : "+r"(ptr) : "r"(end) : XMM16_CLOBBERS);
}

static void k_mem_copy_sse(void* dst, const void* src, u64 bytes, bool nt) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    const u8* end = s + bytes;
    if (nt)
        __asm__ volatile("1:\n\t" SSE_LOAD16("%1") SSE_STORE16("movntdq", "%0")
                         "add $256, %0\n\t" "add $256, %1\n\t" "cmp %2, %1\n\t" "jb 1b\n\t" "sfence\n\t"
                         : "+r"(d), "+r"(s) : "r"(end) : XMM16_CLOBBERS);
    else
        __asm__ volatile("1:\n\t" SSE_LOAD16("%1") SSE_STORE16("movdqa", "%0")
                         "add $256, %0\n\t" "add $256, %1\n\t" "cmp %2, %1\n\t" "jb 1b\n\t"
                         : "+r"(d), "+r"(s) : "r"(end) : XMM16_CLOBBERS);
}

// Triad without FMA: c * s, then + b. 128 bytes per array per iteration.
static void k_mem_triad_sse(f32* a, const f32* b, const f32* c, f32 s, u64 bytes_per_array) {
    u64 i = 0;
    __asm__ volatile(
        "movss %[s], %%xmm15\n\t"
        "shufps $0, %%xmm15, %%xmm15\n\t"
        "1:\n\t"
        "movaps 0(%[c],%[i]), %%xmm0\n\t"  "movaps 16(%[c],%[i]), %%xmm1\n\t"
        "movaps 32(%[c],%[i]), %%xmm2\n\t" "movaps 48(%[c],%[i]), %%xmm3\n\t"
        "movaps 64(%[c],%[i]), %%xmm4\n\t" "movaps 80(%[c],%[i]), %%xmm5\n\t"
        "movaps 96(%[c],%[i]), %%xmm6\n\t" "movaps 112(%[c],%[i]), %%xmm7\n\t"
        "mulps %%xmm15, %%xmm0\n\t" "mulps %%xmm15, %%xmm1\n\t" "mulps %%xmm15, %%xmm2\n\t" "mulps %%xmm15, %%xmm3\n\t"
        "mulps %%xmm15, %%xmm4\n\t" "mulps %%xmm15, %%xmm5\n\t" "mulps %%xmm15, %%xmm6\n\t" "mulps %%xmm15, %%xmm7\n\t"
        "addps 0(%[b],%[i]), %%xmm0\n\t"  "addps 16(%[b],%[i]), %%xmm1\n\t"
        "addps 32(%[b],%[i]), %%xmm2\n\t" "addps 48(%[b],%[i]), %%xmm3\n\t"
        "addps 64(%[b],%[i]), %%xmm4\n\t" "addps 80(%[b],%[i]), %%xmm5\n\t"
        "addps 96(%[b],%[i]), %%xmm6\n\t" "addps 112(%[b],%[i]), %%xmm7\n\t"
        "movaps %%xmm0, 0(%[a],%[i])\n\t"  "movaps %%xmm1, 16(%[a],%[i])\n\t"
        "movaps %%xmm2, 32(%[a],%[i])\n\t" "movaps %%xmm3, 48(%[a],%[i])\n\t"
        "movaps %%xmm4, 64(%[a],%[i])\n\t" "movaps %%xmm5, 80(%[a],%[i])\n\t"
        "movaps %%xmm6, 96(%[a],%[i])\n\t" "movaps %%xmm7, 112(%[a],%[i])\n\t"
        "add $128, %[i]\n\t"
        "cmp %[n], %[i]\n\t"
        "jb 1b\n\t"
        : [i] "+r"(i)
        : [a] "r"(a), [b] "r"(b), [c] "r"(c), [n] "r"(bytes_per_array), [s] "m"(s)
        : XMM16_CLOBBERS);
}

static void k_mem_read_delay_sse(const void* p, u64 bytes, u64 delay) {
    const u8* ptr = (const u8*)p;
    const u8* end = ptr + bytes;
    u64 spin;
    __asm__ volatile(
        "1:\n\t" SSE_LOAD16("%0")
        "mov %3, %1\n\t" "test %1, %1\n\t" "jz 3f\n\t"
        "2:\n\t" "dec %1\n\t" "jnz 2b\n\t"
        "3:\n\t" "add $256, %0\n\t" "cmp %2, %0\n\t" "jb 1b\n\t"
        : "+r"(ptr), "=&r"(spin) : "r"(end), "r"(delay) : XMM16_CLOBBERS);
}

#define SSE_MIX_LOADS                                                                                          \
    "movdqa 0(%1), %%xmm0\n\t"  "movdqa 16(%1), %%xmm1\n\t" "movdqa 32(%1), %%xmm2\n\t" "movdqa 48(%1), %%xmm3\n\t" \
    "movdqa 64(%1), %%xmm4\n\t" "movdqa 80(%1), %%xmm5\n\t" "movdqa 96(%1), %%xmm6\n\t" "movdqa 112(%1), %%xmm7\n\t"
#define SSE_MIX_XOR(O0, O1, O2, O3, O4, O5, O6, O7)                                                            \
    "pxor " O0 "(%1), %%xmm0\n\t" "pxor " O1 "(%1), %%xmm1\n\t" "pxor " O2 "(%1), %%xmm2\n\t"                  \
    "pxor " O3 "(%1), %%xmm3\n\t" "pxor " O4 "(%1), %%xmm4\n\t" "pxor " O5 "(%1), %%xmm5\n\t"                  \
    "pxor " O6 "(%1), %%xmm6\n\t" "pxor " O7 "(%1), %%xmm7\n\t"
#define SSE_MIX_STORES                                                                                         \
    "movdqa %%xmm0, 0(%0)\n\t"  "movdqa %%xmm1, 16(%0)\n\t" "movdqa %%xmm2, 32(%0)\n\t" "movdqa %%xmm3, 48(%0)\n\t" \
    "movdqa %%xmm4, 64(%0)\n\t" "movdqa %%xmm5, 80(%0)\n\t" "movdqa %%xmm6, 96(%0)\n\t" "movdqa %%xmm7, 112(%0)\n\t"

static void k_mem_mix_sse(void* dst, const void* src, u64 dst_bytes, u32 reads) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    const u8* end = d + dst_bytes;
    if (reads <= 1) {
        __asm__ volatile(
            "1:\n\t" SSE_MIX_LOADS SSE_MIX_STORES
            "add $128, %0\n\t" "add $128, %1\n\t" "cmp %2, %0\n\t" "jb 1b\n\t"
            : "+r"(d), "+r"(s) : "r"(end) : XMM16_CLOBBERS);
    } else if (reads == 2) {
        __asm__ volatile(
            "1:\n\t" SSE_MIX_LOADS SSE_MIX_XOR("128", "144", "160", "176", "192", "208", "224", "240") SSE_MIX_STORES
            "add $128, %0\n\t" "add $256, %1\n\t" "cmp %2, %0\n\t" "jb 1b\n\t"
            : "+r"(d), "+r"(s) : "r"(end) : XMM16_CLOBBERS);
    } else {
        __asm__ volatile(
            "1:\n\t" SSE_MIX_LOADS SSE_MIX_XOR("128", "144", "160", "176", "192", "208", "224", "240")
            SSE_MIX_XOR("256", "272", "288", "304", "320", "336", "352", "368") SSE_MIX_STORES
            "add $128, %0\n\t" "add $384, %1\n\t" "cmp %2, %0\n\t" "jb 1b\n\t"
            : "+r"(d), "+r"(s) : "r"(end) : XMM16_CLOBBERS);
    }
}

// ---- exported: AVX2 when the CPU has it ----------------------------------------------------------------------

void k_mem_read(const void* p, u64 bytes) {
    if (g_sys.cpu.avx2) k_mem_read_avx2(p, bytes);
    else k_mem_read_sse(p, bytes);
}
void k_mem_write(void* p, u64 bytes) {
    if (g_sys.cpu.avx2) k_mem_write_avx2(p, bytes);
    else k_mem_write_sse(p, bytes, false);
}
void k_mem_write_nt(void* p, u64 bytes) {
    if (g_sys.cpu.avx2) k_mem_write_nt_avx2(p, bytes);
    else k_mem_write_sse(p, bytes, true);
}
void k_mem_copy(void* dst, const void* src, u64 bytes) {
    if (g_sys.cpu.avx2) k_mem_copy_avx2(dst, src, bytes);
    else k_mem_copy_sse(dst, src, bytes, false);
}
void k_mem_copy_nt(void* dst, const void* src, u64 bytes) {
    if (g_sys.cpu.avx2) k_mem_copy_nt_avx2(dst, src, bytes);
    else k_mem_copy_sse(dst, src, bytes, true);
}
void k_mem_triad(f32* a, const f32* b, const f32* c, f32 s, u64 bytes_per_array) {
    if (g_sys.cpu.avx2 && g_sys.cpu.fma) k_mem_triad_avx2(a, b, c, s, bytes_per_array);
    else k_mem_triad_sse(a, b, c, s, bytes_per_array);
}
void k_mem_read_delay(const void* p, u64 bytes, u64 delay) {
    if (g_sys.cpu.avx2) k_mem_read_delay_avx2(p, bytes, delay);
    else k_mem_read_delay_sse(p, bytes, delay);
}
void k_mem_mix(void* dst, const void* src, u64 dst_bytes, u32 reads) {
    if (g_sys.cpu.avx2) k_mem_mix_avx2(dst, src, dst_bytes, reads);
    else k_mem_mix_sse(dst, src, dst_bytes, reads);
}
