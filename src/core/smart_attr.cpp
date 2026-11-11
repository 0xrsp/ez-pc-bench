// SMART attribute tables (see smart_attr.h).
#include "core/smart_attr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char* const SMART_VENDOR_NAMES[SV_COUNT] = {
    "Generic HDD", "Generic SSD", "Samsung", "Crucial / Micron", "Intel", "WD", "SanDisk", "Kingston", "Phison",
    "SK hynix", "Toshiba / Kioxia SSD", "Seagate", "WD", "Toshiba", "HGST / Hitachi",
};

static const SmartAttrDef GENERIC[] = {
    { 0x01, SU_RAW, "Raw Read Error Rate" },          { 0x02, SU_RAW, "Throughput Performance" },
    { 0x03, SU_RAW, "Spin-Up Time" },                 { 0x04, SU_COUNT, "Start/Stop Count" },
    { 0x05, SU_COUNT, "Reallocated Sectors Count" },  { 0x06, SU_RAW, "Read Channel Margin" },
    { 0x07, SU_RAW, "Seek Error Rate" },              { 0x08, SU_RAW, "Seek Time Performance" },
    { 0x09, SU_HOURS, "Power-On Hours" },             { 0x0A, SU_COUNT, "Spin Retry Count" },
    { 0x0B, SU_COUNT, "Recalibration Retries" },      { 0x0C, SU_COUNT, "Power Cycle Count" },
    { 0x0D, SU_RAW, "Soft Read Error Rate" },         { 0x16, SU_RAW, "Current Helium Level" },
    { 0xAA, SU_RAW, "Available Reserved Space" },     { 0xAB, SU_COUNT, "Program Fail Count" },
    { 0xAC, SU_COUNT, "Erase Fail Count" },           { 0xAD, SU_COUNT, "Wear Leveling Count" },
    { 0xAE, SU_COUNT, "Unexpected Power Loss Count" }, { 0xAF, SU_RAW, "Power Loss Protection Failure" },
    { 0xB0, SU_COUNT, "Erase Fail Count" },           { 0xB1, SU_RAW, "Wear Range Delta" },
    { 0xB3, SU_COUNT, "Used Reserved Block Count" },  { 0xB4, SU_COUNT, "Unused Reserved Block Count" },
    { 0xB5, SU_COUNT, "Program Fail Count" },         { 0xB6, SU_COUNT, "Erase Fail Count" },
    { 0xB7, SU_COUNT, "SATA Downshift Error Count" }, { 0xB8, SU_COUNT, "End-to-End Error" },
    { 0xBB, SU_COUNT, "Reported Uncorrectable Errors" }, { 0xBC, SU_COUNT48, "Command Timeout" },
    { 0xBD, SU_COUNT, "High Fly Writes" },            { 0xBE, SU_TEMP, "Airflow Temperature" },
    { 0xBF, SU_COUNT, "G-Sense Error Rate" },         { 0xC0, SU_COUNT, "Power-off Retract Count" },
    { 0xC1, SU_COUNT, "Load/Unload Cycle Count" },    { 0xC2, SU_TEMP, "Temperature" },
    { 0xC3, SU_RAW, "Hardware ECC Recovered" },       { 0xC4, SU_COUNT, "Reallocation Event Count" },
    { 0xC5, SU_COUNT, "Current Pending Sector Count" }, { 0xC6, SU_COUNT, "Uncorrectable Sector Count" },
    { 0xC7, SU_COUNT, "UltraDMA CRC Error Count" },   { 0xC8, SU_COUNT, "Write Error Rate" },
    { 0xC9, SU_RAW, "Soft Read Error Rate" },         { 0xCA, SU_COUNT, "Data Address Mark Errors" },
    { 0xCB, SU_RAW, "Run Out Cancel" },               { 0xCC, SU_COUNT, "Soft ECC Correction" },
    { 0xCD, SU_COUNT, "Thermal Asperity Rate" },      { 0xCE, SU_RAW, "Flying Height" },
    { 0xCF, SU_RAW, "Spin High Current" },            { 0xD0, SU_RAW, "Spin Buzz" },
    { 0xD1, SU_RAW, "Offline Seek Performance" },     { 0xDC, SU_RAW, "Disk Shift" },
    { 0xDD, SU_COUNT, "G-Sense Error Rate" },         { 0xDE, SU_HOURS, "Loaded Hours" },
    { 0xDF, SU_COUNT, "Load/Unload Retry Count" },    { 0xE0, SU_RAW, "Load Friction" },
    { 0xE1, SU_COUNT, "Load/Unload Cycle Count" },    { 0xE2, SU_RAW, "Load-in Time" },
    { 0xE3, SU_COUNT, "Torque Amplification Count" }, { 0xE4, SU_COUNT, "Power-Off Retract Cycle" },
    { 0xE6, SU_RAW, "GMR Head Amplitude" },           { 0xE7, SU_TEMP, "Temperature" },
    { 0xE8, SU_RAW, "Endurance Remaining" },          { 0xE9, SU_HOURS, "Power-On Hours" },
    { 0xF0, SU_HOURS, "Head Flying Hours" },          { 0xF1, SU_LBA, "Total LBAs Written" },
    { 0xF2, SU_LBA, "Total LBAs Read" },              { 0xF3, SU_RAW, "Total LBAs Written Expanded" },
    { 0xF4, SU_RAW, "Total LBAs Read Expanded" },     { 0xF9, SU_GIB, "NAND Writes (1 GiB)" },
    { 0xFA, SU_COUNT, "Read Error Retry Rate" },      { 0xFE, SU_COUNT, "Free Fall Protection" },
};

// SSDs without a vendor table, and the fallback for vendor tables.
static const SmartAttrDef SSD[] = {
    { 0xA9, SU_LIFE, "Remaining Life" },              { 0xAD, SU_COUNT, "Average Erase Count" },
    { 0xE6, SU_RAW, "Life Curve Status" },            { 0xE7, SU_LIFE, "SSD Life Left" },
    { 0xE8, SU_RAW, "Available Reserved Space" },     { 0xE9, SU_LIFE, "Media Wearout Indicator" },
};

static const SmartAttrDef SAMSUNG[] = {
    { 0x05, SU_COUNT, "Reallocated Sector Count" },   { 0xB1, SU_LIFE, "Wear Leveling Count" },
    { 0xB3, SU_COUNT, "Used Reserved Block Count (Total)" }, { 0xB5, SU_COUNT, "Program Fail Count (Total)" },
    { 0xB6, SU_COUNT, "Erase Fail Count (Total)" },   { 0xB7, SU_COUNT, "Runtime Bad Block (Total)" },
    { 0xBB, SU_COUNT, "Uncorrectable Error Count" },  { 0xC3, SU_RAW, "ECC Error Rate" },
    { 0xC7, SU_COUNT, "CRC Error Count" },            { 0xEB, SU_COUNT, "POR Recovery Count" },
};

static const SmartAttrDef CRUCIAL[] = {
    { 0x05, SU_COUNT, "Reallocated NAND Block Count" }, { 0xAA, SU_COUNT, "Grown Failing Block Count" },
    { 0xAD, SU_COUNT, "Average Block-Erase Count" },  { 0xB4, SU_COUNT, "Unused Reserve NAND Blocks" },
    { 0xB7, SU_COUNT, "SATA Interface Downshift" },   { 0xB8, SU_COUNT, "Error Correction Count" },
    { 0xC5, SU_COUNT, "Current Pending ECC Count" },  { 0xC6, SU_COUNT, "Offline Uncorrectable" },
    { 0xCA, SU_LIFE, "Percent Lifetime Remaining" },  { 0xCE, SU_COUNT, "Write Error Rate" },
    { 0xD2, SU_COUNT, "Successful RAIN Recovery Count" }, { 0xF6, SU_LBA, "Total Host Sectors Written" },
    { 0xF7, SU_COUNT48, "Host Program Page Count" },  { 0xF8, SU_COUNT48, "FTL Program Page Count" },
};

static const SmartAttrDef INTEL[] = {
    { 0xAA, SU_RAW, "Available Reserved Space" },     { 0xB8, SU_COUNT, "End-to-End Error Detection Count" },
    { 0xBE, SU_TEMP, "Case Temperature" },            { 0xC0, SU_COUNT, "Unsafe Shutdown Count" },
    { 0xC2, SU_TEMP, "Internal Temperature" },        { 0xC7, SU_COUNT, "CRC Error Count" },
    { 0xE1, SU_32MIB, "Host Writes" },                { 0xE2, SU_RAW, "Timed Workload Media Wear" },
    { 0xE3, SU_RAW, "Timed Workload Host Read %" },   { 0xE4, SU_MINUTES, "Timed Workload Timer" },
    { 0xE8, SU_RAW, "Available Reserved Space" },     { 0xE9, SU_LIFE, "Media Wearout Indicator" },
    { 0xEA, SU_RAW, "Thermal Throttle Status" },      { 0xF1, SU_32MIB, "Host Writes" },
    { 0xF2, SU_32MIB, "Host Reads" },                 { 0xF9, SU_GIB, "NAND Writes (1 GiB)" },
};

static const SmartAttrDef WD_SSD[] = {
    { 0xA5, SU_COUNT, "Total Write/Erase Count" },    { 0xA6, SU_COUNT, "Min Write/Erase Count" },
    { 0xA7, SU_COUNT, "Min Bad Block/Die" },          { 0xA8, SU_COUNT, "Max Bad Block/Die" },
    { 0xA9, SU_LIFE, "Percent Life Remaining" },      { 0xAA, SU_COUNT, "Grown Bad Blocks" },
    { 0xAD, SU_COUNT, "Average Write/Erase Count" },  { 0xE6, SU_RAW, "Life Curve Status" },
    { 0xE8, SU_RAW, "Available Reserved Space" },     { 0xE9, SU_GIB, "NAND GiB Written" },
    { 0xEA, SU_GIB, "NAND GiB Written (SLC)" },       { 0xF1, SU_GIB, "Host GiB Written" },
    { 0xF2, SU_GIB, "Host GiB Read" },                { 0xF4, SU_RAW, "Temperature Throttle Status" },
};

static const SmartAttrDef PHISON[] = {     // Phison controllers (Kingston, Patriot, Corsair, PNY, Silicon Power...)
    { 0xA8, SU_COUNT, "SATA PHY Error Count" },       { 0xA9, SU_RAW, "Bad Block Rate" },
    { 0xAA, SU_COUNT, "Bad Block Count" },            { 0xAD, SU_COUNT, "Erase Count" },
    { 0xE7, SU_LIFE, "SSD Life Left" },               { 0xE9, SU_GIB, "Lifetime Flash Writes" },
    { 0xEA, SU_GIB, "Lifetime Flash Writes (SLC)" },  { 0xF1, SU_GIB, "Lifetime Host Writes" },
    { 0xF2, SU_GIB, "Lifetime Host Reads" },
};

static const SmartAttrDef SEAGATE[] = {
    { 0x01, SU_RAW, "Raw Read Error Rate" },          { 0x07, SU_RAW, "Seek Error Rate" },
    { 0xBC, SU_COUNT48, "Command Timeout" },          { 0xBD, SU_COUNT, "High Fly Writes" },
    { 0xBE, SU_TEMP, "Airflow Temperature" },         { 0xC3, SU_RAW, "Hardware ECC Recovered" },
    { 0xF0, SU_HOURS, "Head Flying Hours" },          { 0xF1, SU_LBA, "Total LBAs Written" },
    { 0xF2, SU_LBA, "Total LBAs Read" },
};

struct VendorTable {
    const SmartAttrDef* defs;
    u32 count;
};
#define VT(a) { a, ARRAY_COUNT(a) }
static const VendorTable VENDOR_TABLES[SV_COUNT] = {
    { nullptr, 0 }, { nullptr, 0 }, VT(SAMSUNG), VT(CRUCIAL), VT(INTEL), VT(WD_SSD), VT(WD_SSD), VT(PHISON), VT(PHISON),
    { nullptr, 0 }, { nullptr, 0 }, VT(SEAGATE), { nullptr, 0 }, { nullptr, 0 }, { nullptr, 0 },
};
#undef VT

static bool is_ssd_vendor(SmartVendor v) {
    return v == SV_GENERIC_SSD || v == SV_SAMSUNG || v == SV_CRUCIAL || v == SV_INTEL || v == SV_WD_SSD || v == SV_SANDISK ||
           v == SV_KINGSTON || v == SV_PHISON || v == SV_SKHYNIX || v == SV_TOSHIBA_SSD;
}

SmartVendor smart_vendor(const char* model, bool ssd) {
    char m[64];
    u32 n = 0;
    for (; model[n] && n + 1 < sizeof(m); ++n) m[n] = (char)toupper((unsigned char)model[n]);
    m[n] = 0;
    auto has = [&](const char* s) { return strstr(m, s) != nullptr; };
    auto starts = [&](const char* s) { return strncmp(m, s, strlen(s)) == 0; };
    if (has("SAMSUNG")) return ssd ? SV_SAMSUNG : SV_GENERIC_HDD;
    if (has("CRUCIAL") || has("MICRON") || starts("MTFD") || (starts("CT") && isdigit((unsigned char)m[2]))) return SV_CRUCIAL;
    if (has("INTEL")) return SV_INTEL;
    if (has("SANDISK")) return SV_SANDISK;
    if (starts("WDC WDS") || starts("WDS") || has("WD BLUE SA") || has("WD RED SA") || has("WD GREEN")) return SV_WD_SSD;
    if (has("KINGSTON")) return SV_KINGSTON;
    if (has("PATRIOT") || has("CORSAIR FORCE") || has("PNY CS") || has("SILICON POWER") || has("GOODRAM")) return SV_PHISON;
    if (has("SK HYNIX") || starts("HFS")) return SV_SKHYNIX;
    if (has("TOSHIBA") || has("KIOXIA")) return ssd ? SV_TOSHIBA_SSD : SV_TOSHIBA_HDD;
    if (starts("ST") && isdigit((unsigned char)m[2])) return SV_SEAGATE;
    if (starts("WDC WD") || (starts("WD") && isdigit((unsigned char)m[2]))) return SV_WD_HDD;
    if (has("HGST") || has("HITACHI")) return SV_HGST;
    return ssd ? SV_GENERIC_SSD : SV_GENERIC_HDD;
}

static const SmartAttrDef* find(const SmartAttrDef* defs, u32 count, u8 id) {
    for (u32 i = 0; i < count; ++i)
        if (defs[i].id == id) return &defs[i];
    return nullptr;
}

const SmartAttrDef* smart_attr_def(SmartVendor vendor, u8 id) {
    const VendorTable* t = &VENDOR_TABLES[vendor < SV_COUNT ? vendor : 0];
    if (const SmartAttrDef* d = t->defs ? find(t->defs, t->count, id) : nullptr) return d;
    if (is_ssd_vendor(vendor))
        if (const SmartAttrDef* d = find(SSD, ARRAY_COUNT(SSD), id)) return d;
    if (const SmartAttrDef* d = find(GENERIC, ARRAY_COUNT(GENERIC), id)) return d;
    static const SmartAttrDef unknown = { 0, SU_RAW, "Vendor Specific" };
    return &unknown;
}

u64 smart_raw48(const u8 raw[6]) {
    u64 v = 0;
    for (u32 i = 0; i < 6; ++i) v |= (u64)raw[i] << (8 * i);
    return v;
}

u64 smart_bytes(SmartUnit unit, const u8 raw[6]) {
    const u64 v = smart_raw48(raw);
    switch (unit) {
        case SU_LBA: return v * 512ull;
        case SU_32MIB: return v * 32ull * 1024 * 1024;
        case SU_GIB: return v * 1024ull * 1024 * 1024;
        default: return 0;
    }
}

static void bytes_text(u64 b, char* out, u32 size) {
    if (b >= 1000000000000ull) snprintf(out, size, "%.2f TB", b / 1e12);
    else snprintf(out, size, "%.1f GB", b / 1e9);
}

void smart_decode(SmartUnit unit, const u8 raw[6], u8 current, char* out, u32 size) {
    const u32 lo32 = (u32)(raw[0] | (raw[1] << 8) | (raw[2] << 16) | ((u32)raw[3] << 24));
    out[0] = 0;
    switch (unit) {
        case SU_COUNT: snprintf(out, size, "%u", lo32); break;
        case SU_COUNT48: snprintf(out, size, "%llu", smart_raw48(raw)); break;
        case SU_HOURS: snprintf(out, size, "%u h", lo32); break;
        case SU_MINUTES: snprintf(out, size, "%u min", lo32); break;
        case SU_TEMP: {
            const u8 t = raw[0], lo = raw[2], hi = raw[4] ? raw[4] : raw[3];
            if (lo && hi && lo <= t && t <= hi && hi < 128)
                snprintf(out, size, "%u \xC2\xB0" "C (min %u, max %u)", t, lo, hi);
            else
                snprintf(out, size, "%u \xC2\xB0" "C", t);
            break;
        }
        case SU_LBA:
        case SU_32MIB:
        case SU_GIB: bytes_text(smart_bytes(unit, raw), out, size); break;
        case SU_LIFE: snprintf(out, size, "%u %% left", current); break;
        case SU_LIFE_USED: snprintf(out, size, "%u %% used", lo32); break;
        default: break;
    }
}
