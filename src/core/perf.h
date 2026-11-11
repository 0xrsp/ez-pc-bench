#pragma once
// Process/thread tweaks applied for the duration of a run.

void perf_begin_run();      // priority, power-throttling opt-out, timer resolution, keep awake
void perf_end_run();        // restores everything perf_begin_run changed
void perf_setup_worker_thread();    // per benchmark thread: priority + throttling opt-out
