// AMD memory controller (UMC) timings through SMN reads (PawnIO AMDFamily17 ioctl_read_smn). Read-only.
//
// Register offsets and bit fields as used by ZenTimings / ZenStates-Core (irusanov, GPL-3.0; cited, not copied):
// ZenTimings WPF-no-themes/MainWindow.xaml.cs ReadTimings(), ZenStates-Core Hardware/DRAM/MemoryConfig.cs
// (channel / DIMM / rank discovery) and Ddr5Timings.cs (DDR5 tRFC / tRFCsb registers). Channel n lives at
// SMN 0x50000 + (n << 20). UNTESTED against the real driver (PawnIO wasn't installed on the development machine).
#include "core/adv_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

constexpr u32 UMC_BASE = 0x50000;
constexpr u32 UMC_MAX_CHANNELS = 12;

static u32 bits(u32 v, u32 lo, u32 width) { return (v >> lo) & ((1u << width) - 1); }

static bool rd(u32 ch_off, u32 reg, u32* v) {
    return amd_smn_read(ch_off | reg, v) && *v != 0xFFFFFFFFu;
}

void advumc_read(MemTimings* t, const AdvCpu* first) {
    memset(t, 0, sizeof(*t));
    t->mts = t->mclk_mhz = t->fclk_mhz = t->uclk_mhz = NAN;
    if (!g_adv.amd || !g_adv.cpu.h || g_adv.family < 0x17) return;

    // Channels: enabled when 0x50DF0 bit 19 is clear and a DIMM's chip select 0 / 2 base (0x50000 / 0x50008)
    // has bit 0 set. Timings are read from the first enabled channel (all channels train to the same values).
    u32 first_off = UINT32_MAX;
    for (u32 ch = 0; ch < UMC_MAX_CHANNELS; ++ch) {
        const u32 off = ch << 20;
        u32 ctl = 0, cs0 = 0, cs2 = 0;
        if (!rd(off, UMC_BASE + 0xDF0, &ctl) || (ctl & (1u << 19))) continue;
        if (!rd(off, UMC_BASE + 0x00, &cs0) || !rd(off, UMC_BASE + 0x08, &cs2)) continue;
        const u32 dimms = (cs0 & 1) + (cs2 & 1);
        if (!dimms) continue;
        ++t->channels;
        t->dimms += dimms;
        if (first_off == UINT32_MAX) {
            first_off = off;
            u32 cs1 = 0;    // ranks of the first DIMM: chip selects 0 and 1 in use
            t->ranks = (cs0 & 1) + (rd(off, UMC_BASE + 0x04, &cs1) ? (cs1 & 1) : 0);
        }
    }
    if (first_off == UINT32_MAX) {
        snprintf(t->note, sizeof(t->note), "no enabled memory channel found");
        return;
    }

    u32 r[32] = {};
    static const u32 regs[] = { 0x100, 0x12C, 0x200, 0x204, 0x208, 0x20C, 0x210, 0x214, 0x218, 0x21C, 0x220, 0x224,
                                0x228, 0x230, 0x234, 0x250, 0x254, 0x258, 0x260, 0x264, 0x268, 0x26C, 0x2C0, 0x2C4,
                                0x2C8, 0x2CC };
    for (u32 i = 0; i < ARRAY_COUNT(regs); ++i)
        if (!amd_smn_read(first_off | (UMC_BASE + regs[i]), &r[i])) {
            snprintf(t->note, sizeof(t->note), "SMN read of UMC register 0x%X failed", UMC_BASE + regs[i]);
            return;
        }
    const u32 config = r[0], power = r[1], base = r[2], t5 = r[3], t6 = r[4], t7 = r[5], t8 = r[6], t9 = r[7],
              t10 = r[8], t15 = r[13];

    static const char* const types[] = { "DDR4", "DDR5", "LPDDR4", "LPDDR5" };
    const u32 type = bits(config, 0, 2);
    snprintf(t->type, sizeof(t->type), "%s", types[type]);
    snprintf(t->source, sizeof(t->source), "AMD UMC");
    const bool ddr4 = type == 0;
    if (type >= 2) {        // LPDDR: other clocking (WCK) and register meanings; not decoded
        snprintf(t->note, sizeof(t->note), "%s timings not decoded", t->type);
        return;
    }
    // Clock ratio: DDR4 [6:0] / 3, DDR5 [15:0] / 100; x 200 = MT/s.
    const f32 ratio = ddr4 ? bits(base, 0, 7) / 3.0f : bits(base, 0, 16) / 100.0f;
    t->mts = ratio * 200.0f;
    t->mclk_mhz = ratio * 100.0f;
    t->cmd2t = bits(base, ddr4 ? 10 : 17, 1) != 0;
    t->gdm = bits(base, ddr4 ? 11 : 18, 1) != 0;
    t->powerdown = bits(power, 28, 1) != 0;

    t->cl = bits(t5, 0, 6);
    t->ras = bits(t5, 8, 7);
    t->rcdrd = bits(t5, 16, 6);
    t->rcdwr = bits(t5, 24, 6);
    t->rc = bits(t6, 0, 8);
    t->rp = bits(t6, 16, 6);
    t->rrds = bits(t7, 0, 5);
    t->rrdl = bits(t7, 8, 5);
    t->rtp = bits(t7, 24, 5);
    t->faw = bits(t8, 0, 7);
    t->cwl = bits(t9, 0, 6);
    t->wtrs = bits(t9, 8, 5);
    t->wtrl = bits(t9, 16, 7);
    t->wr = bits(t10, 0, 7);
    t->refi = bits(t15, 0, 16);

    // tRFC: DDR4 picks the trained one of two registers (the other holds a 0x21060138 default); DDR5 the first
    // of four that isn't the 0x00C00138 default.
    const u32* rfc = &r[18];
    u32 rfc_reg = 0;
    if (ddr4) rfc_reg = rfc[0] != rfc[1] ? (rfc[0] != 0x21060138 ? rfc[0] : rfc[1]) : rfc[0];
    else
        for (u32 i = 0; i < 4 && !rfc_reg; ++i)
            if (rfc[i] && rfc[i] != 0x00C00138) rfc_reg = rfc[i];
    if (ddr4) {
        t->rfc = bits(rfc_reg, 0, 11);
        t->rfc2 = bits(rfc_reg, 11, 11);
    } else {
        t->rfc = bits(rfc_reg, 0, 16);
        t->rfc2 = bits(rfc_reg, 16, 16);
        for (u32 i = 22; i < 26 && !t->rfcsb; ++i) t->rfcsb = bits(r[i], 0, 11);
    }
    if (first) {
        t->fclk_mhz = first->fclk_mhz;
        t->uclk_mhz = first->uclk_mhz;
    }
    // Plausibility: a failed or unsupported read shows up as zero CAS latency or an absurd clock.
    t->valid = t->cl > 0 && t->mts > 800.0f && t->mts < 20000.0f;
    if (!t->valid) snprintf(t->note, sizeof(t->note), "implausible register values (CL %u, %.0f MT/s)", t->cl, t->mts);
}
