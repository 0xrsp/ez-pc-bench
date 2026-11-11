#pragma once
#include "core/types.h"
#include <intrin.h>

// QueryPerformanceCounter based wall clock.
u64 timer_ticks();          // raw QPC ticks
f64 timer_tick_seconds();   // seconds per tick
inline f64 timer_now() { return (f64)timer_ticks() * timer_tick_seconds(); }   // seconds since boot

inline u64 rdtsc() { return __rdtsc(); }

// Keep a value alive so the optimizer can't delete the work that produced it.
template <class T> inline void do_not_optimize(T const& v) { __asm__ volatile("" : : "r,m"(v) : "memory"); }
inline void clobber_memory() { __asm__ volatile("" : : : "memory"); }
