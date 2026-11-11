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

constexpr u64 KiB = 1024ull;
constexpr u64 MiB = 1024ull * KiB;
constexpr u64 GiB = 1024ull * MiB;
