#pragma once
// ATA SMART attribute names and raw-value units: a generic table plus vendor overrides (static arrays), picked
// by the drive's model string. Names follow the vendors' documentation / smartmontools' drive database.
#include "core/types.h"

enum SmartVendor : u8 {
    SV_GENERIC_HDD, SV_GENERIC_SSD, SV_SAMSUNG, SV_CRUCIAL, SV_INTEL, SV_WD_SSD, SV_SANDISK, SV_KINGSTON, SV_PHISON,
    SV_SKHYNIX, SV_TOSHIBA_SSD, SV_SEAGATE, SV_WD_HDD, SV_TOSHIBA_HDD, SV_HGST, SV_COUNT
};
extern const char* const SMART_VENDOR_NAMES[SV_COUNT];

// How to show the raw value.
enum SmartUnit : u8 {
    SU_RAW,         // hex only
    SU_COUNT,       // low 32 bits
    SU_COUNT48,     // all 48 bits
    SU_HOURS,       // low 32 bits, hours
    SU_TEMP,        // byte 0 = °C (bytes 2 / 4 = min / max on some drives)
    SU_LBA,         // 512-byte sectors
    SU_32MIB,       // units of 32 MiB
    SU_GIB,         // GiB
    SU_LIFE,        // the normalized current value is the remaining life in %
    SU_LIFE_USED,   // the raw value is the used life in %
    SU_MINUTES,
};

struct SmartAttrDef {
    u8 id;
    SmartUnit unit;
    const char* name;
};

SmartVendor smart_vendor(const char* model, bool ssd);
// Name and unit of attribute `id` for `vendor` (generic fallback; "Vendor specific" if unknown).
const SmartAttrDef* smart_attr_def(SmartVendor vendor, u8 id);
// Raw value (6 bytes, little endian) of an attribute.
u64 smart_raw48(const u8 raw[6]);
// Human-readable raw value per unit ("12 345", "38 °C (min 20, max 51)", "15.2 TB").
void smart_decode(SmartUnit unit, const u8 raw[6], u8 current, char* out, u32 size);
// Bytes for SU_LBA / SU_32MIB / SU_GIB attributes.
u64 smart_bytes(SmartUnit unit, const u8 raw[6]);
