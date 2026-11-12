#pragma once
// WHEA monitor: counts Windows Hardware Error Architecture events (Microsoft-Windows-WHEA-Logger in the System
// log) as they are logged, via EvtSubscribe. Corrected errors (machine-check, PCIe, memory ECC corrections)
// are logged as warnings / information, uncorrected ones as errors. A fatal error usually resets the PC and is
// logged at the next boot, so a live count only ever sees what the system survived.
//
// Usage (stability tester, and the benchmark runner if it wants a count per run): whea_start() once, take a
// snapshot when the run begins and diff it at the end. Counters are cumulative since the first whea_start().
#include "core/types.h"

struct WheaCounts {
    u32 corrected;          // levels warning / information
    u32 uncorrected;        // levels critical / error
    u32 last_event_id;      // WHEA-Logger event id of the newest event (17 PCIe, 19 machine check, 47 memory, ...)
    f64 last_time;          // timer_now() when it arrived, 0 if none
    bool active;            // the subscription is running (false: no access to the System log, or not started)
};

void whea_start();          // reference-counted; safe to call from any thread
void whea_stop();
void whea_snapshot(WheaCounts* out);
// Counts WHEA-Logger events already in the System log (oldest record first). For reports and as a self-test of
// the query. False if the log can't be read.
bool whea_history(u32* corrected, u32* uncorrected);
