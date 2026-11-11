#include "core/perf.h"
#include <windows.h>
#include <timeapi.h>

static DWORD s_prev_priority_class;

void perf_begin_run() {
    HANDLE proc = GetCurrentProcess();
    s_prev_priority_class = GetPriorityClass(proc);
    SetPriorityClass(proc, HIGH_PRIORITY_CLASS);

    // Opt out of EcoQoS / power throttling even when the window is in the background.
    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pt.StateMask = 0;
    SetProcessInformation(proc, ProcessPowerThrottling, &pt, sizeof(pt));

    timeBeginPeriod(1);
    // Called on the worker thread; perf_end_run must run on the same thread.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
}

void perf_end_run() {
    SetThreadExecutionState(ES_CONTINUOUS);
    timeEndPeriod(1);

    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;   // ControlMask 0: let the system decide again
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));

    SetPriorityClass(GetCurrentProcess(), s_prev_priority_class ? s_prev_priority_class : NORMAL_PRIORITY_CLASS);
}

void perf_setup_worker_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    THREAD_POWER_THROTTLING_STATE tt = {};
    tt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    tt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    tt.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tt, sizeof(tt));
}
