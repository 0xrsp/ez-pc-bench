#pragma once
// Charts-tab views for memory results that need more than a line plot: the AIDA-style cache & memory grid
// (mem.grid) and the loaded-latency curve, latency vs bandwidth (mem.lat.loaded).
#include "core/bench.h"

bool mem_chart_has_view(u32 bench);                 // test `bench` has a dedicated view (also for values results)
void mem_chart_draw(u32 bench, const BenchResult* r);
