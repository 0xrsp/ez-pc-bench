#pragma once
// RAM details read through PawnIO at startup (adv_init, see adv_sensors.h): what the modules' SPD says (over the
// SMBus) and the memory controller's current timings (AMD UMC registers). Empty without PawnIO; everything here
// then prints / writes nothing.
#include "core/types.h"
#include <stdio.h>

enum SpdType : u8 { SPD_NONE, SPD_DDR4, SPD_DDR5 };

// Read-only: the SPD hub / EEPROM shows one page of its SPD at a time and switching pages is an SMBus write,
// which we don't do. So only the page the firmware left selected is decoded (normally page 0: the JEDEC base
// configuration). Manufacturer / part number come from SMBIOS (sysinfo.h); XMP / EXPO profiles (pages 5-7 on
// DDR5, page 1 on DDR4) and the DDR5 manufacturing block (page 4) stay unread. See docs/notes/pawnio.md.
struct SpdModule {
    u8 addr;                    // SMBus address 0x50-0x57
    SpdType type;
    bool temp_sensor;           // DDR5 hub with an enabled temperature sensor / DDR4 TSE2004 sensor answered
    u8 page;                    // SPD page that was readable (DDR5 hub MR11[2:0]), 0 on DDR4
    bool base_ok;               // JEDEC base configuration decoded (density, speed, timings)
    u32 die_mbit;               // density per die
    u8 dies;                    // dies per package
    char package[12];           // "Monolithic", "DDP", "2H 3DS"
    u8 width;                   // device I/O width: 4 / 8 / 16
    u8 ranks;                   // package ranks (DDR4 base config), 0 = unknown
    u32 jedec_mts;              // fastest JEDEC speed (from tCKmin)
    u32 jedec_cl;               // ceil(tAA / tCK) at that speed
    f32 taa_ns, trcd_ns, trp_ns, tras_ns, trc_ns, trfc1_ns;
    char note[96];
};

struct MemTimings {
    bool valid;
    char type[8];               // "DDR5", "DDR4", "LPDDR5"
    char source[16];            // "AMD UMC"
    u32 channels;               // enabled memory channels
    u32 dimms;                  // DIMMs seen by the controller
    u32 ranks;                  // ranks of the first DIMM (chip selects in use), 0 = unknown
    f32 mts, mclk_mhz;          // from the controller's clock ratio
    f32 fclk_mhz, uclk_mhz;     // from the SMU PM table where its layout is known; NaN otherwise
    u32 cl, rcdrd, rcdwr, rp, ras, rc, rrds, rrdl, faw, wtrs, wtrl, wr, cwl, rtp, rfc, rfc2, rfcsb, refi;   // clocks
    bool cmd2t, gdm, powerdown;
    char note[96];
};

struct MemDetails {
    bool read;                  // the PawnIO read ran
    u32 spd_count;
    SpdModule spd[8];
    char spd_note[160];         // why there is no SPD data (SMBus locked, no answer, ...)
    MemTimings timings;
};
extern MemDetails g_memdetail;

// "DDR5-5600 CL46-45-45-90, tRFC 884 (316 ns), 1T, GDM off, 2 channels", false (buf "") without data.
bool memdetail_timings_line(char* buf, u32 size);
// One line per module: "SPD 0x50: DDR5, 16 Gbit dies x16 monolithic, JEDEC 5600 CL46-45-45, sensor"; returns the count.
u32 memdetail_spd_lines(char lines[][192], u32 max);
// system.ram.timings / system.ram.spd in the results file; writes nothing without data.
void memdetail_write_toon(FILE* f, u32 depth);
