#pragma once
// Live hardware readings for the Sensors window, the overlay and the sensor log, sampled by one background
// thread while anyone holds a reference (sensors_start / sensors_stop, counted):
//   - CPU / system: ACPI thermal zones ("Thermal Zone Information" performance counters). On most laptops
//     one of them tracks the CPU package; exact per-core temperatures need a kernel driver, which we don't use.
//   - CPU clock / load per logical processor and GPU clocks / load / VRAM / power (core/hwread.*).
//   - GPUs: core temperature, fan speed and power from D3DKMT adapter perf data (what Task Manager shows).
//   - Drives: the drive's own sensor via IOCTL_STORAGE_QUERY_PROPERTY (NVMe composite temperature).
//   - Battery charge / discharge rate, voltage, level (core/battery.*) and Energy Metering Interface power
//     rails such as CPU package / per-core power (core/emi.*).
// Every value is also published as a flat list of channels (id, name, unit, now / min / max / avg, one-second
// history) that the sensor log and the overlay use. More sources (e.g. PawnIO CPU / RAM temperatures) add
// channels in publish() without changing this interface.
#include "core/battery.h"
#include "core/hwread.h"

constexpr u32 SENSOR_MAX = 32;
constexpr u32 SENSOR_HISTORY = 120;     // samples (seconds)

enum SensorGroup : u8 { SG_CPU, SG_GPU, SG_DISK, SG_COUNT };

struct SensorReading {
    SensorGroup group;
    char name[96];
    char detail[96];            // source / extra values ("ACPI \_TZ.TZ01", "fan 2100 RPM, power 35%")
    bool valid;                 // has a current temperature
    f32 temp_c, min_c, max_c;   // min / max since the window opened
    f32 warn_c;                 // warning threshold (driver-reported or a sensible default)
    f32 history[SENSOR_HISTORY];    // oldest first, history_count valid entries
    u32 history_count;
    char note[160];             // why there's no value, if !valid
};

// Live clocks and load (Sensors window), sampled on the same thread.
struct CpuLive {
    u32 lps;                                // 0 until the second sample
    f32 util[HW_MAX_LPS], mhz[HW_MAX_LPS];  // per logical processor; mhz NaN = not reported this time
    f32 avg_util;                           // all LPs
    f32 busy_mhz;                           // utilization-weighted clock of the LPs >= 50 % busy (NaN if none)
    f32 max_mhz;                            // highest LP clock since the window opened
    f32 util_hist[SENSOR_HISTORY];
    u32 hist_count;
};

struct GpuLive {
    char name[128];
    bool opened;                            // D3DKMT adapter handle available
    GpuReading r;
    f32 vram_total_mib;
    f32 util_hist[SENSOR_HISTORY];
    u32 hist_count;
};

void sensors_start();           // takes a reference; the first one starts the sampling thread
void sensors_stop();            // drops it; the last one stops the thread
// Copies the latest readings; returns how many. `updated` (optional): seconds since the last sample.
u32 sensors_snapshot(SensorReading* out, u32 max, f64* updated);
// Copies the live CPU / GPU readings; returns the GPU count (gpus: room for 8).
u32 sensors_live(CpuLive* cpu, GpuLive* gpus);

// ---- power: battery and energy meters ------------------------------------------------------------------

constexpr u32 SENSOR_RAILS = 48;

struct PowerLive {
    u32 battery_count;
    BatteryInfo battery[BATTERY_MAX];       // static info + latest status
    u32 rail_count;                         // EMI channels
    char rail_name[SENSOR_RAILS][64];       // "CPU package", "CPU core 3", ...
    char rail_raw[SENSOR_RAILS][64];        // as the meter names it ("PPM RAPL_Package0_PKG")
    f32 rail_w[SENSOR_RAILS];               // NaN until the second read
    char emi_status[160];                   // why there are no rails ("" when there are)
    bool read;                              // sources opened (false until the first sample)
};
void sensors_power(PowerLive* out);

// ---- channels ------------------------------------------------------------------------------------------

constexpr u32 CH_MAX = 192;
constexpr u32 CH_HISTORY = 600;             // one point per second: 10 minutes

enum ChannelGroup : u8 { CHG_CPU, CHG_LP, CHG_GPU, CHG_TEMP, CHG_POWER, CHG_COUNT };
extern const char* const g_channel_group_names[CHG_COUNT];

enum ChannelFlags : u8 {
    CHF_DETAIL = 1u << 0,       // per-LP / per-core values: left out of logs and lists unless asked for
};

struct SensorChannel {
    char id[40];                // stable on a machine: "cpu.load", "gpu0.core_mhz", "temp.tz01", "bat.rate_w", "emi.cpu_pkg"
    char name[96];
    char label[24];             // short form for the overlay: "CPU clock", "GPU0 temp", "Core 3"
    char unit[8];               // "%", "MHz", "\xC2\xB0""C", "W", "V", "Wh", "MiB", "RPM"
    ChannelGroup group;
    u8 flags;                   // ChannelFlags
    u8 decimals;                // for display and logs
    bool valid;                 // has a current value
    f32 value;
    f32 min, max;               // since the start or the last sensors_reset_stats (NaN if no value yet)
    f64 sum;                    // avg = sum / count
    u32 count;
};

// Copies the channel list; returns the count. `seq` (optional): sample counter, changes on every sample.
u32 sensors_channels(SensorChannel* out, u32 max, u32* seq = nullptr);
// One-second history of channel `ch`, oldest first, NaN where there was no value. Returns the point count.
u32 sensors_channel_history(u32 ch, f32* out, u32 max);
void sensors_reset_stats();     // min / max / avg of every channel start over

// '*' wildcard match of a channel id against a comma-separated pattern list ("cpu.*,gpu0.*,temp.*").
// Empty list: every channel without CHF_DETAIL. "all": every channel.
bool sensors_channel_matches(const SensorChannel* c, const char* patterns);

// ---- log -----------------------------------------------------------------------------------------------
// One row per interval with the current value of each chosen channel, written by the sampling thread
// (which then samples at the log interval when that is below one second; minimum 100 ms).
//   CSV:  UTF-8 with BOM; "Time,Seconds,<name> [<unit>],..." then one row per interval, empty = no value.
//   TOON: app / kind: sensor-log / timestamp / interval_ms / cpu, channels[N]{id,name,unit}, then
//         samples[M]{t,<channel ids>} (t = seconds since the first row, null = no value). Rows go to
//         "<file>.rows.tmp" while logging; sensors_log_stop writes the file and deletes the temp file.
// Channels are fixed at the first row. Holds a sensors reference while active.

enum SensorLogFormat : u8 { SLOG_CSV, SLOG_TOON };

struct SensorLogStatus {
    bool active;
    SensorLogFormat format;
    u32 rows, columns, interval_ms;
    f64 seconds;                // since the first row
    char path[520];             // UTF-8
    char error[160];            // last error ("" if none)
};

// `patterns` as for sensors_channel_matches. False (with err) if the file can't be created.
bool sensors_log_start(const wchar_t* path, SensorLogFormat format, u32 interval_ms, const char* patterns, char* err,
                       u32 err_size);
void sensors_log_stop();        // finishes the file; no-op if not logging
void sensors_log_status(SensorLogStatus* out);
