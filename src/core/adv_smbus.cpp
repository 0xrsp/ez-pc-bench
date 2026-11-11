// DIMM temperature sensors and SPD over the chipset SMBus through PawnIO's SmbusPIIX4 (AMD FCH) / SmbusI801
// (Intel PCH) modules. Read transactions only (SMBus "read byte / word data"); we never select another SPD page
// or SMBus port, never write a register. See sysinfo_spd.h for what that leaves unread.
//
// References (cited, not copied): JEDEC JESD300-5 SPD5118 hub (MR0/MR1 device type, MR11 legacy page / addressing,
// MR26 sensor disable, MR49/MR50 temperature: bits [12:2] signed, 0.25 C), JESD400-5 DDR5 SPD base configuration,
// JEDEC SPD annex L (DDR4), JC-42.4 / TSE2004 sensor (register 5, MSB first, 1/16 C); register map as in
// ZenStates-Core (GPL-3.0) Hardware/DRAM/DDR5/Hub/Spd5118Registers.cs and Linux drivers/hwmon/spd5118.c / jc42.c.
//
// Laptops: the embedded controller often shares or owns this bus. Any timeout, collision or "busy" answer marks
// the bus unusable for the rest of the session so we never fight the EC; the note says why. UNTESTED against
// the real driver (PawnIO wasn't installed on the development machine).
#include "core/adv_internal.h"
#include "core/timer.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

constexpr u64 SMBUS_READ = 1;
constexpr u64 PROTO_BYTE_DATA = 2, PROTO_WORD_DATA = 3;     // I2C_SMBUS_* (Linux numbering, as the modules use)
constexpr u8 SPD_FIRST = 0x50, TS_DDR4_FIRST = 0x18;
// SPD5118 mode registers (MR0 / MR1, the device type, are read as bytes 0 / 1 of the first probe).
constexpr u8 MR5 = 0x05, MR11 = 0x0B, MR26 = 0x1A, MR49 = 0x31, MR50 = 0x32;

enum XferResult : u8 { XF_OK, XF_NO_DEVICE, XF_BUS_ERROR };

static struct {
    bool unusable;              // stop touching the bus (EC conflict / errors)
    u32 count;
    AdvDimm dimm[ADV_MAX_DIMMS];
    u8 fails[ADV_MAX_DIMMS];
} s;

static XferResult classify(u32 err) {
    if (!err) return XF_OK;
    // STATUS_NO_SUCH_DEVICE (no ACK) -> ERROR_FILE_NOT_FOUND; anything else (timeout, busy, collision) is a bus problem.
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_DEV_NOT_EXIST ? XF_NO_DEVICE : XF_BUS_ERROR;
}

static XferResult read_data(u8 addr, u8 cmd, u64 proto, u32* value) {
    const u64 in[4] = { addr, SMBUS_READ, cmd, proto };
    u64 out[1] = {};
    const XferResult r = classify(pawnio_call(&g_adv.smbus, "ioctl_smbus_xfer", in, 4, out, 1));
    if (r == XF_BUS_ERROR) s.unusable = true;
    *value = (u32)out[0];
    return r;
}

static XferResult read_byte(u8 addr, u8 cmd, u8* v) {
    u32 x = 0;
    const XferResult r = read_data(addr, cmd, PROTO_BYTE_DATA, &x);
    *v = (u8)x;
    return r;
}

// ---- sensors --------------------------------------------------------------------------------------------

static bool spd5118_id(u8 mr0, u8 mr1) {
    // 0x51 0x18 = SPD5118 hub with temperature sensor; some controllers return the pair swapped.
    return (mr0 == 0x51 && (mr1 & 0x0F) == 0x08) || ((mr0 & 0x0F) == 0x08 && mr1 == 0x51);
}

static bool ddr5_temp(u8 addr, f32* t) {
    u8 lo = 0, hi = 0;
    if (read_byte(addr, MR49, &lo) != XF_OK || read_byte(addr, MR50, &hi) != XF_OK) return false;
    i32 v = (((u32)hi << 8 | lo) >> 2) & 0x7FF;
    if (v & 0x400) v -= 0x800;
    *t = (f32)v * 0.25f;
    return *t > -40.0f && *t < 125.0f;
}

static bool ddr4_temp(u8 addr, f32* t) {
    u32 w = 0;
    if (read_data(addr, 0x05, PROTO_WORD_DATA, &w) != XF_OK) return false;
    const u32 raw = ((w & 0xFF) << 8) | ((w >> 8) & 0xFF);     // JC-42.4 sends the MSB first
    i32 v = (i32)(raw & 0x1FFF);
    if (v & 0x1000) v -= 0x2000;
    *t = (f32)v * 0.0625f;
    return *t > -40.0f && *t < 125.0f;
}

// ---- SPD decoding (base configuration only) -------------------------------------------------------------

static u32 u16le(const u8* b, u32 i) { return b[i] | (u32)b[i + 1] << 8; }

static u32 speed_bin(f64 mts, const u32* bins, u32 n) {
    u32 best = 0;
    for (u32 i = 0; i < n; ++i)
        if (bins[i] <= mts * 1.01) best = bins[i];
    return best;
}

static u32 cl_for(f32 taa_ns, f32 tck_ns) { return tck_ns > 0.0f ? (u32)ceilf(taa_ns / tck_ns - 0.01f) : 0; }

// JESD400-5 bytes 0-127: density / package / width at 4-6, timings in ps (16-bit LE) from byte 20, tRFC in ns.
static void decode_ddr5(const u8* b, SpdModule* m) {
    static const u32 bins[] = { 3200, 3600, 4000, 4400, 4800, 5200, 5600, 6000, 6400, 6800, 7200, 7600, 8000, 8400, 8800 };
    static const u32 dens[] = { 0, 4096, 8192, 12288, 16384, 24576, 32768, 49152, 65536 };
    static const char* const pkgs[] = { "Monolithic", "DDP", "2H 3DS", "4H 3DS", "8H 3DS", "16H 3DS" };
    static const u8 dies[] = { 1, 2, 2, 4, 8, 16 };
    if (b[2] != 0x12) { snprintf(m->note, sizeof(m->note), "SPD byte 2 = 0x%02X, not DDR5", b[2]); return; }
    const u32 d = b[4] & 0x1F, p = (b[4] >> 5) & 7;
    m->die_mbit = d < ARRAY_COUNT(dens) ? dens[d] : 0;
    m->dies = p < ARRAY_COUNT(dies) ? dies[p] : 0;
    snprintf(m->package, sizeof(m->package), "%s", p < ARRAY_COUNT(pkgs) ? pkgs[p] : "?");
    m->width = (u8)(((b[6] >> 5) & 7) <= 3 ? 4 << ((b[6] >> 5) & 7) : 0);
    const u32 tck = u16le(b, 20);
    if (!tck) return;
    m->jedec_mts = speed_bin(2e6 / tck, bins, ARRAY_COUNT(bins));
    m->taa_ns = u16le(b, 30) / 1000.0f;
    m->trcd_ns = u16le(b, 32) / 1000.0f;
    m->trp_ns = u16le(b, 34) / 1000.0f;
    m->tras_ns = u16le(b, 36) / 1000.0f;
    m->trc_ns = u16le(b, 38) / 1000.0f;
    m->trfc1_ns = (f32)u16le(b, 42);
    m->jedec_cl = cl_for(m->taa_ns, tck / 1000.0f);
    m->base_ok = m->die_mbit && m->jedec_mts;
}

// DDR4 annex L: medium timebase 125 ps + signed fine correction in ps.
static f32 ddr4_t(const u8* b, u32 mtb_lo, u32 fine, u32 mtb_hi_nibble = 0) {
    return ((f32)(mtb_lo | mtb_hi_nibble << 8) * 125.0f + (f32)(i8)b[fine]) / 1000.0f;
}

static void decode_ddr4(const u8* b, SpdModule* m) {
    static const u32 bins[] = { 1600, 1866, 2133, 2400, 2666, 2933, 3200 };
    static const u32 dens[] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 12288, 24576 };
    const u32 d = b[4] & 0x0F;
    m->die_mbit = d < ARRAY_COUNT(dens) ? dens[d] : 0;
    m->dies = (u8)(((b[6] >> 4) & 7) + 1);
    snprintf(m->package, sizeof(m->package), "%s", !(b[6] & 0x80) ? "Monolithic" : (b[6] & 3) == 2 ? "3DS" : "Multi-die");
    m->width = (u8)(4 << (b[12] & 7));
    m->ranks = (u8)(((b[12] >> 3) & 7) + 1);
    const f32 tck = ddr4_t(b, b[18], 125);
    if (tck <= 0.0f) return;
    m->jedec_mts = speed_bin(2000.0 / tck, bins, ARRAY_COUNT(bins));
    m->taa_ns = ddr4_t(b, b[24], 123);
    m->trcd_ns = ddr4_t(b, b[25], 122);
    m->trp_ns = ddr4_t(b, b[26], 121);
    m->tras_ns = (f32)(b[28] | (b[27] & 0x0F) << 8) * 0.125f;
    m->trc_ns = ddr4_t(b, b[29], 120, (b[27] >> 4) & 0x0F);
    m->trfc1_ns = (f32)u16le(b, 30) * 0.125f;
    m->jedec_cl = cl_for(m->taa_ns, tck);
    m->base_ok = m->die_mbit && m->jedec_mts;
}

// ---- probing --------------------------------------------------------------------------------------------

static void probe_ddr5(u8 addr, SpdModule* m) {
    m->type = SPD_DDR5;
    u8 mr5 = 0, mr11 = 0, mr26 = 0;
    if (read_byte(addr, MR5, &mr5) != XF_OK || read_byte(addr, MR11, &mr11) != XF_OK || read_byte(addr, MR26, &mr26) != XF_OK)
        return;
    m->temp_sensor = (mr5 & 0x02) && !(mr26 & 0x01);
    if (!(mr5 & 0x02)) snprintf(m->note, sizeof(m->note), "hub without temperature sensor");
    else if (mr26 & 0x01) snprintf(m->note, sizeof(m->note), "temperature sensor disabled by the firmware");
    if (mr11 & 0x08) {      // 2-byte addressing: the 1-byte register window doesn't map to the SPD
        snprintf(m->note, sizeof(m->note), "SPD hub in 2-byte addressing mode, contents not read");
        return;
    }
    m->page = mr11 & 7;
    if (m->page != 0) {
        snprintf(m->note, sizeof(m->note), "firmware left SPD page %u selected; base configuration is on page 0", m->page);
        return;
    }
    u8 nvm[128];
    for (u32 i = 0; i < 128; ++i)
        if (read_byte(addr, (u8)(0x80 + i), &nvm[i]) != XF_OK) return;
    decode_ddr5(nvm, m);
}

static void probe_ddr4(u8 addr, const u8* first, SpdModule* m) {
    m->type = SPD_DDR4;
    u8 spd[256];
    memcpy(spd, first, 3);
    for (u32 i = 3; i < 256; ++i)
        if (read_byte(addr, (u8)i, &spd[i]) != XF_OK) return;
    decode_ddr4(spd, m);
    // The module's TSE2004 sensor, if fitted, sits at 0x18 + the same slot bits. Confirm it by its
    // manufacturer / device ID registers before trusting it (laptops sometimes have other parts at 0x18-0x1F).
    const u8 ts = (u8)(TS_DDR4_FIRST + (addr - SPD_FIRST));
    u32 mfg = 0, dev = 0;
    f32 t;
    if (read_data(ts, 0x06, PROTO_WORD_DATA, &mfg) == XF_OK && read_data(ts, 0x07, PROTO_WORD_DATA, &dev) == XF_OK &&
        mfg != 0 && mfg != 0xFFFF && ddr4_temp(ts, &t))
        m->temp_sensor = true;
}

void advmem_setup(MemDetails* d) {
    memset(&s, 0, sizeof(s));
    if (!g_adv.smbus.h) return;
    if (!bus_lock(LOCK_SMBUS, 500)) {
        snprintf(d->spd_note, sizeof(d->spd_note), "SMBus in use by another program; DIMM sensors not read");
        return;
    }
    u32 answered = 0;
    for (u8 i = 0; i < 8 && !s.unusable && d->spd_count < ARRAY_COUNT(d->spd); ++i) {
        const u8 addr = (u8)(SPD_FIRST + i);
        u8 first[3];
        const XferResult r = read_byte(addr, 0, &first[0]);
        if (r != XF_OK) continue;
        ++answered;
        if (read_byte(addr, 1, &first[1]) != XF_OK || read_byte(addr, 2, &first[2]) != XF_OK) continue;
        SpdModule* m = &d->spd[d->spd_count++];
        memset(m, 0, sizeof(*m));
        m->addr = addr;
        if (spd5118_id(first[0], first[1])) probe_ddr5(addr, m);
        else if (first[2] == 0x0C) probe_ddr4(addr, first, m);
        else snprintf(m->note, sizeof(m->note), "unknown device (0x%02X 0x%02X 0x%02X)", first[0], first[1], first[2]);
        if (m->temp_sensor && s.count < ADV_MAX_DIMMS) {
            AdvDimm* dm = &s.dimm[s.count++];
            dm->addr = m->type == SPD_DDR5 ? addr : (u8)(TS_DDR4_FIRST + i);
            dm->ddr5 = m->type == SPD_DDR5;
        }
    }
    bus_unlock(LOCK_SMBUS);
    if (s.unusable)
        snprintf(d->spd_note, sizeof(d->spd_note), "SMBus errors (busy / timeout): it is probably shared with or locked "
                                                   "by the embedded controller, so EzPcBench stopped using it");
    else if (!answered)
        snprintf(d->spd_note, sizeof(d->spd_note), "no SPD device answered on the chipset SMBus (common on laptops, "
                                                   "where the memory's SPD bus isn't wired to it)");
}

void advmem_sample(AdvMem* out) {
    memset(out, 0, sizeof(*out));
    if (s.unusable || !s.count || !g_adv.smbus.h) return;
    if (!bus_lock(LOCK_SMBUS, 50)) return;
    for (u32 i = 0; i < s.count && !s.unusable; ++i) {
        AdvDimm* d = &s.dimm[i];
        if (s.fails[i] >= 3) { d->valid = false; continue; }     // gave up on this one
        f32 t = NAN;
        d->valid = d->ddr5 ? ddr5_temp(d->addr, &t) : ddr4_temp(d->addr, &t);
        d->temp_c = d->valid ? t : NAN;
        s.fails[i] = d->valid ? 0 : (u8)(s.fails[i] + 1);
    }
    bus_unlock(LOCK_SMBUS);
    out->count = s.count;
    memcpy(out->dimms, s.dimm, s.count * sizeof(AdvDimm));
    out->time = timer_now();
}

void advmem_close() { memset(&s, 0, sizeof(s)); }
