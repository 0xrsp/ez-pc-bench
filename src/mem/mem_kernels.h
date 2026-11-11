#pragma once
// Memory kernels (inline asm, AVX2). Pointers 64-byte aligned, byte counts multiples of 256.
#include "core/types.h"

void k_mem_read(const void* p, u64 bytes);
void k_mem_write(void* p, u64 bytes);
void k_mem_write_nt(void* p, u64 bytes);                    // non-temporal (streaming) stores
void k_mem_copy(void* dst, const void* src, u64 bytes);
void k_mem_copy_nt(void* dst, const void* src, u64 bytes);
void k_mem_triad(f32* a, const f32* b, const f32* c, f32 s, u64 bytes_per_array);   // a = b + s*c
void* k_mem_chase(void* p, u64 steps);                      // p = *p, `steps` multiple of 16
