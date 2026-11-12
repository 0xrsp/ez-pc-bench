// RAM SPD / timings formatting and results-file output (see sysinfo_spd.h). The data is gathered by adv_init
// through PawnIO (adv_smbus.cpp, adv_umc.cpp).
#include "core/sysinfo_spd.h"
#include "core/toon.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

MemDetails g_memdetail;

bool memdetail_timings_line(char* buf, u32 size) {
    buf[0] = 0;
    const MemTimings* t = &g_memdetail.timings;
    if (!t->valid) return false;
    const f32 rfc_ns = t->mclk_mhz > 0.0f ? t->rfc * 1000.0f / t->mclk_mhz : NAN;
    int n = snprintf(buf, size, "%s-%.0f CL%u-%u-%u-%u, tRFC %u (%.0f ns), %s, GDM %s", t->type, t->mts, t->cl, t->rcdrd,
                     t->rp, t->ras, t->rfc, rfc_ns, t->cmd2t ? "2T" : "1T", t->gdm ? "on" : "off");
    if (n > 0 && (u32)n < size && !isnan(t->fclk_mhz))
        n += snprintf(buf + n, size - n, ", FCLK %.0f MHz", t->fclk_mhz);
    if (n > 0 && (u32)n < size && !isnan(t->uclk_mhz))
        n += snprintf(buf + n, size - n, ", UCLK %.0f MHz (%s)", t->uclk_mhz,
                      fabsf(t->uclk_mhz - t->mclk_mhz) < 0.05f * t->mclk_mhz ? "1:1" : "1:2");
    if (n > 0 && (u32)n < size)
        snprintf(buf + n, size - n, ", %u channel%s%s", t->channels, t->channels == 1 ? "" : "s",
                 t->ranks ? (t->ranks == 1 ? ", 1 rank" : ", 2 ranks") : "");
    return true;
}

u32 memdetail_spd_lines(char lines[][192], u32 max) {
    u32 n = 0;
    for (u32 i = 0; i < g_memdetail.spd_count && n < max; ++i) {
        const SpdModule* m = &g_memdetail.spd[i];
        const char* type = m->type == SPD_DDR5 ? "DDR5" : m->type == SPD_DDR4 ? "DDR4" : "?";
        char* l = lines[n++];
        int k = snprintf(l, 192, "SPD 0x%02X: %s", m->addr, type);
        if (m->base_ok && k > 0 && k < 192) {
            const f32 tck_ns = 2000.0f / m->jedec_mts;      // clocks at the JEDEC speed, rounded up like tAA -> CL
            const u32 rcd = (u32)ceilf(m->trcd_ns / tck_ns - 0.01f), rp = (u32)ceilf(m->trp_ns / tck_ns - 0.01f);
            k += snprintf(l + k, 192 - k, ", %u Gbit x%u %s%s, JEDEC %u CL%u-%u-%u", m->die_mbit / 1024, m->width,
                          m->package, m->ranks == 2 ? ", 2 ranks" : m->ranks == 1 ? ", 1 rank" : "", m->jedec_mts,
                          m->jedec_cl, rcd, rp);
        }
        if (m->temp_sensor && k > 0 && k < 192) k += snprintf(l + k, 192 - k, ", temperature sensor");
        if (m->note[0] && k > 0 && k < 192) snprintf(l + k, 192 - k, " (%s)", m->note);
    }
    return n;
}

void memdetail_write_toon(FILE* f, u32 depth) {
    const MemTimings* t = &g_memdetail.timings;
    if (t->valid) {
        toon_key(f, depth, "timings");
        toon_str(f, depth + 1, "source", t->source);
        toon_str(f, depth + 1, "type", t->type);
        toon_num(f, depth + 1, "mts", t->mts);
        toon_num(f, depth + 1, "mclk_mhz", t->mclk_mhz);
        if (!isnan(t->fclk_mhz)) toon_num(f, depth + 1, "fclk_mhz", t->fclk_mhz);
        if (!isnan(t->uclk_mhz)) toon_num(f, depth + 1, "uclk_mhz", t->uclk_mhz);
        toon_num(f, depth + 1, "channels", t->channels);
        toon_num(f, depth + 1, "ranks", t->ranks);
        toon_str(f, depth + 1, "cmd_rate", t->cmd2t ? "2T" : "1T");
        toon_bool(f, depth + 1, "gdm", t->gdm);
        toon_bool(f, depth + 1, "power_down", t->powerdown);
        const struct { const char* k; u32 v; } clk[] = {
            { "cl", t->cl }, { "rcdrd", t->rcdrd }, { "rcdwr", t->rcdwr }, { "rp", t->rp }, { "ras", t->ras },
            { "rc", t->rc }, { "rrds", t->rrds }, { "rrdl", t->rrdl }, { "faw", t->faw }, { "wtrs", t->wtrs },
            { "wtrl", t->wtrl }, { "wr", t->wr }, { "cwl", t->cwl }, { "rtp", t->rtp }, { "rfc", t->rfc },
            { "rfc2", t->rfc2 }, { "rfcsb", t->rfcsb }, { "refi", t->refi },
        };
        for (u32 i = 0; i < ARRAY_COUNT(clk); ++i) toon_num(f, depth + 1, clk[i].k, clk[i].v);
    }
    if (!g_memdetail.spd_count) return;
    toon_table(f, depth, "spd", g_memdetail.spd_count,
               "addr,type,die_mbit,dies,package,width,ranks,jedec_mts,jedec_cl,taa_ns,trcd_ns,trp_ns,tras_ns,trfc1_ns,temp_sensor");
    for (u32 i = 0; i < g_memdetail.spd_count; ++i) {
        const SpdModule* m = &g_memdetail.spd[i];
        ToonRow r = toon_row(f, depth + 1);
        toon_cell_num(&r, m->addr);
        toon_cell_str(&r, m->type == SPD_DDR5 ? "DDR5" : m->type == SPD_DDR4 ? "DDR4" : "unknown");
        toon_cell_num(&r, m->die_mbit);
        toon_cell_num(&r, m->dies);
        toon_cell_str(&r, m->package);
        toon_cell_num(&r, m->width);
        toon_cell_num(&r, m->ranks);
        toon_cell_num(&r, m->jedec_mts);
        toon_cell_num(&r, m->jedec_cl);
        toon_cell_num(&r, m->taa_ns);
        toon_cell_num(&r, m->trcd_ns);
        toon_cell_num(&r, m->trp_ns);
        toon_cell_num(&r, m->tras_ns);
        toon_cell_num(&r, m->trfc1_ns);
        toon_cell_bool(&r, m->temp_sensor);
        toon_row_end(&r);
    }
}
