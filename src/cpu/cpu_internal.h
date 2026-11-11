#pragma once
// Shared declarations for the CPU suite's kernel files.
#include "core/types.h"

// ---- cpu_kernels.cpp: inline-asm throughput loops. Each runs `iters` loop iterations and
// returns the work done (integer ops or FLOPs).
u64 k_int_alu(u64 iters);       // 12 independent add/xor per iteration
u64 k_int_mul(u64 iters);       // 6 independent imul per iteration
u64 k_fma_ss(u64 iters);        // 12 scalar FP32 FMA
u64 k_fma_ps256(u64 iters);     // 12 x 8-lane FP32 FMA
u64 k_fma_pd256(u64 iters);     // 12 x 4-lane FP64 FMA
u64 k_fma_ps512(u64 iters);     // 12 x 16-lane FP32 FMA (needs AVX-512F)
u64 k_fma_pd512(u64 iters);     // 12 x 8-lane FP64 FMA (needs AVX-512F)

// ---- cpu_crypto.cpp
void sha256_blocks(u32 state[8], const u8* data, u64 len);    // SHA-NI, len multiple of 64
bool sha256_selftest();

struct alignas(16) AesKey { u8 rk[11 * 16]; };
void aes128_expand(const u8 key[16], AesKey* out);
// CTR-mode style encryption (64-bit little-endian counter increment), bytes multiple of 256.
void aes_ctr_ni(const AesKey* k, u64 ctr, const u8* src, u8* dst, u64 bytes);
void aes_ctr_vaes512(const AesKey* k, u64 ctr, const u8* src, u8* dst, u64 bytes);
bool aes_selftest(bool check_vaes512);

// ---- cpu_gemm.cpp: C = A * B, square FP32 matrices, AVX2 6x16 micro-kernel.
constexpr u32 GEMM_N = 1920;
struct GemmCtx;
GemmCtx* gemm_create(u32 max_threads);
void gemm_destroy(GemmCtx* g);
f64 gemm_run_part(GemmCtx* g, u32 index, u32 count);   // computes this thread's rows; returns FLOPs
f64 gemm_max_error(const GemmCtx* g);                   // spot-check vs double-precision reference
