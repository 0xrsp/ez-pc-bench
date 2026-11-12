#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// The build baseline is x86-64-v2 (SSE4.2 + POPCNT, CMakeLists.txt). Code that needs more is compiled per
// function with one of these and only called after checking the CPU (g_sys.cpu, BenchDesc::isa).
// TARGET_AVX2 is the Haswell / Zen set that /arch:AVX2 used to enable for everything.
#define TARGET_AVX2 __attribute__((target("avx2,fma,bmi,bmi2,lzcnt,f16c,movbe")))
#define TARGET_FMA  __attribute__((target("fma")))
// Plain C++ kernels the compiler vectorizes: body in a FORCE_INLINE function, inlined into a TARGET_AVX2 copy
// and a baseline copy, picked at run time, so AVX2 CPUs keep the AVX2 code (and the same results as before).
#define FORCE_INLINE __attribute__((always_inline)) inline

constexpr u64 KiB = 1024ull;
constexpr u64 MiB = 1024ull * KiB;
constexpr u64 GiB = 1024ull * MiB;
