#pragma once
// Memory kernels (inline asm, AVX2 or SSE2 by CPU). Pointers 64-byte aligned, byte counts multiples of 256.
#include "core/types.h"

void k_mem_read(const void* p, u64 bytes);
void k_mem_write(void* p, u64 bytes);
void k_mem_write_nt(void* p, u64 bytes);                    // non-temporal (streaming) stores
void k_mem_copy(void* dst, const void* src, u64 bytes);
void k_mem_copy_nt(void* dst, const void* src, u64 bytes);
void k_mem_triad(f32* a, const f32* b, const f32* c, f32 s, u64 bytes_per_array);   // a = b + s*c
void* k_mem_chase(void* p, u64 steps);                      // p = *p, `steps` multiple of 16
// Reads 256 bytes, then spins `delay` iterations of a dec loop (~1 cycle each), until `bytes` are read.
// Throttled load generator for the loaded-latency test.
void k_mem_read_delay(const void* p, u64 bytes, u64 delay);
// Read / write mix: for every 128 bytes stored to dst, reads `reads` x 128 bytes from src (reads = 1, 2, 3;
// the stored data is the XOR of what was read). dst_bytes multiple of 128.
void k_mem_mix(void* dst, const void* src, u64 dst_bytes, u32 reads);
