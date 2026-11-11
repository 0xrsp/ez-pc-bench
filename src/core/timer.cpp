#include "core/timer.h"
#include <windows.h>

static f64 s_tick_seconds = 0.0;

u64 timer_ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

f64 timer_tick_seconds() {
    if (s_tick_seconds == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_tick_seconds = 1.0 / (f64)f.QuadPart;
    }
    return s_tick_seconds;
}
