#pragma once
// Energy Metering Interface: power rails a platform exposes through energy-meter devices
// (GUID_DEVICE_ENERGY_METER, IOCTL_EMI_*; e.g. Intel's RAPL package / core / graphics rails, PMIC rails on
// Arm and some AMD platforms). Each channel reports accumulated energy (pWh) and a timestamp; power is the
// difference between two reads. Many PCs expose no meter at all: emi_open then returns 0 with `status` set.
#include "core/types.h"

constexpr u32 EMI_MAX_DEVICES = 32;
constexpr u32 EMI_MAX_CHANNELS = 48;

struct EmiChannel {
    char name[64];              // readable: "CPU package", "CPU core 3", else the raw name
    char raw[64];               // "<model> <channel>" as the meter reports it, e.g. "PPM RAPL_Package0_PKG"
    char id[32];                // short stable id for logs: "cpu_pkg", "core3", ...
    u32 order;                  // channels are sorted by this (package, domains, cores by number, others)
    u8 device;
    u16 index;                  // channel index within its device
    u64 energy_pwh, time_100ns; // last read
    f32 watts;                  // average since the previous read; NaN until the second read
};

struct EmiReader {
    u32 device_count;
    void* handle[EMI_MAX_DEVICES];      // HANDLE
    u16 version[EMI_MAX_DEVICES];
    u16 channels_of[EMI_MAX_DEVICES];
    u32 channel_count;
    EmiChannel ch[EMI_MAX_CHANNELS];
    char status[160];                   // why there are no channels, "" if there are
};

u32 emi_open(EmiReader* r);     // returns the channel count
void emi_close(EmiReader* r);
void emi_read(EmiReader* r);    // updates every channel's watts
