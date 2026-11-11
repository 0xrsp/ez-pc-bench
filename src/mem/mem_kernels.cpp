// Memory kernels in inline asm (AT&T syntax): 256 bytes per loop iteration, 8 ymm registers.
#include "mem/mem_kernels.h"

#define YMM_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory"

void k_mem_read(const void* p, u64 bytes) {
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

void k_mem_write(void* p, u64 bytes) {
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

void k_mem_write_nt(void* p, u64 bytes) {
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

void k_mem_copy(void* dst, const void* src, u64 bytes) {
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

void k_mem_copy_nt(void* dst, const void* src, u64 bytes) {
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
void k_mem_triad(f32* a, const f32* b, const f32* c, f32 s, u64 bytes_per_array) {
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
