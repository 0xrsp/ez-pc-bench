#pragma once
// Laptop battery via the battery class driver (IOCTL_BATTERY_QUERY_TAG / _INFORMATION / _STATUS on the
// GUID_DEVCLASS_BATTERY device interfaces; no admin needed). The charge / discharge rate doubles as a system
// power meter while on battery. Values the battery doesn't report stay NaN / 0.
//
// The status comes from the ACPI _BST method (embedded controller); firmware refreshes it every few seconds,
// so polling faster than ~1 Hz just returns the same numbers.
#include "core/types.h"

constexpr u32 BATTERY_MAX = 4;

enum BatteryState : u32 {
    BAT_ONLINE      = 1u << 0,      // on AC power
    BAT_DISCHARGING = 1u << 1,
    BAT_CHARGING    = 1u << 2,
    BAT_CRITICAL    = 1u << 3,
};

struct BatteryStatus {
    u32 state;                  // BatteryState
    f32 rate_w;                 // + charging, - discharging; NaN unknown
    f32 voltage_v;              // NaN unknown
    f32 capacity_wh;            // remaining; NaN unknown
    f32 charge_pct;             // remaining / full-charge capacity; NaN unknown
    f32 temp_c;                 // NaN: most laptop batteries don't report it
};

struct BatteryInfo {
    char name[64], manufacturer[64], serial[32];
    char chemistry[8];          // "LION", "LiP", ...
    bool relative;              // capacities are relative (%) instead of mWh: rate / Wh unknown
    f32 design_wh, full_wh;     // design vs full-charge capacity; 0 unknown
    f32 wear_pct;               // 100 - full / design; NaN unknown
    u32 cycle_count;            // 0 = not reported
    BatteryStatus status;
};

struct BatteryReader {
    u32 count;
    void* handle[BATTERY_MAX];  // HANDLE
    u32 tag[BATTERY_MAX];
    bool has_temp[BATTERY_MAX];         // BatteryTemperature answered at open (most don't: not asked again)
    BatteryInfo info[BATTERY_MAX];      // static part filled by battery_open
};

// Opens every present battery and reads its static information. Returns the battery count.
u32 battery_open(BatteryReader* r);
void battery_close(BatteryReader* r);
// Refreshes info[i].status for every battery. Returns false if no battery answered.
bool battery_read(BatteryReader* r);
// Sum over batteries: discharge rate in W (positive while discharging, NaN on AC / unknown).
f32 battery_discharge_w(const BatteryInfo* info, u32 count);

// Background poller: reads the status every `period_ms` on its own thread. Under all-core load
// IOCTL_BATTERY_QUERY_STATUS blocked for 2.4 s here (the ACPI / embedded-controller work runs on starved
// system threads), so sampling threads must never call battery_read themselves.
struct BatteryPoller {
    BatteryReader r;            // owned by the poller thread after start
    BatteryInfo info[BATTERY_MAX];      // latest copy (lock)
    u32 count;
    bool has_temp[BATTERY_MAX];
    u32 period_ms;
    void* thread;               // HANDLE
    void* stop;                 // HANDLE (event)
    void* lock;                 // SRWLOCK storage
};
u32 battery_poll_start(BatteryPoller* p, u32 period_ms);    // opens the batteries; returns their count
void battery_poll_stop(BatteryPoller* p);
u32 battery_poll_get(BatteryPoller* p, BatteryInfo* out);   // latest info + status; returns the count
