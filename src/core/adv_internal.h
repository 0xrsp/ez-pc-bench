#pragma once
// Shared between the adv_*.cpp files (see adv_sensors.h). After adv_init, only the sampling thread touches
// the driver handles (and init / the CLI report while that thread isn't running).
#include "core/adv_sensors.h"
#include "core/pawnio.h"
#include "core/sysinfo_spd.h"

struct AdvCtx {
    Pawn cpu;           // AMDFamily17 (AMD) or IntelMSR (Intel)
    Pawn smu;           // RyzenSMU (AMD): PM table
    Pawn smbus;         // SmbusPIIX4 (AMD FCH) or SmbusI801 (Intel PCH)
    bool amd, intel;
    u32 family, model;
};
extern AdvCtx g_adv;

// adv_cpu.cpp
void advcpu_setup(AdvInfo* info);           // after the modules are open: units, P0 clock, PM table address
void advcpu_close();
void advcpu_sample(AdvCpu* out, bool hop);  // hop = allowed to visit every logical processor (no benchmark running)
bool amd_smn_read(u32 addr, u32* value);    // AMD SMN register via AMDFamily17 (takes the PCI bus lock)
u32 advcpu_pm_raw(f32* out, u32 max);       // whole PM table page as floats for the report; returns the count

// adv_smbus.cpp
void advmem_setup(MemDetails* d);           // probe the DIMM sensors / SPD once (read-only)
void advmem_sample(AdvMem* out);            // DIMM temperatures
void advmem_close();

// adv_umc.cpp
void advumc_read(MemTimings* t, const AdvCpu* first);   // AMD memory controller timings; first: PM table clocks
