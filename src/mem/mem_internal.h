#pragma once
// Shared by the memory suite's files (mem_bench.cpp, mem_grid.cpp, mem_load.cpp).
#include "core/bench.h"
#include "core/throughput.h"

struct MemState {
    u8* arena;      // suite arena: min(1.5 GiB, 1/4 of free RAM), 4 KiB pages, pre-faulted
    u64 bytes;
};

// Phase length for tests made of many short points: duration / div, at least 0.25 s (shorter points got
// noisy on battery while the clock ramps).
f64 mem_point_seconds(const BenchParams* p, f64 div);

// Pointer chase: links the 64-byte lines of buf[0, bytes) into one random cycle (Sattolo's shuffle).
// perm needs bytes / 64 entries.
void chase_build(u8* buf, u64 bytes, u32* perm, u64* rng);
// Follows the cycle starting at buf for `seconds` (after a short warm-up); ns per hop, 0 if cancelled.
f64 chase_ns(u8* buf, u64 bytes, f64 seconds, const CancelFlag* cancel);
