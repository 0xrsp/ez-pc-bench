// Drive health (see disk_health.h).
#include "core/disk_health.h"
#include "core/csmi.h"
#include <windows.h>
#include <winioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

const char* const HEALTH_STATUS_NAMES[4] = { "Unknown", "Good", "Caution", "Bad" };

// Caution limits (CrystalDiskInfo defaults): any reallocated (05), pending (C5) or uncorrectable (C6) sector or
// NVMe media error, or 10 % of rated endurance left.
constexpr i64 CAUTION_COUNT = 1;
constexpr i32 CAUTION_LIFE_LEFT_PCT = 10;

static CsmiDisk s_csmi[CSMI_MAX];
static u32 s_csmi_count;

// ---- small helpers -----------------------------------------------------------------------------------

static u16 le16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 le64(const u8* p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }

static void cat(char* buf, u32 size, const char* fmt, ...) {
    const size_t len = strlen(buf);
    if (len + 1 >= size) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf + len, size - len, fmt, a);
    va_end(a);
}

// Appends ", name" to a comma-separated list.
static void list_add(char* buf, u32 size, const char* name) { cat(buf, size, "%s%s", buf[0] ? ", " : "", name); }

static void add_reason(DiskHealth* h, HealthStatus level, const char* fmt, ...) {
    if (level > h->status) h->status = level;
    char text[160];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof(text), fmt, a);
    va_end(a);
    cat(h->status_text, sizeof(h->status_text), "%s%s", h->status_text[0] ? "; " : "", text);
}

// NVMe identify strings: space padded, not terminated.
static void fixed_string(const u8* p, u32 len, char* out, u32 size) {
    u32 n = 0;
    for (u32 i = 0; i < len && n + 1 < size; ++i) out[n++] = p[i] >= ' ' && p[i] < 0x7F ? (char)p[i] : ' ';
    out[n] = 0;
    while (n && out[n - 1] == ' ') out[--n] = 0;
    u32 s = 0;
    while (out[s] == ' ') ++s;
    if (s) memmove(out, out + s, n - s + 1);
}

static void bytes_text(u64 b, char* out, u32 size) {
    if (b >= 1000000000000ull) snprintf(out, size, "%.2f TB", b / 1e12);
    else snprintf(out, size, "%.1f GB", b / 1e9);
}

static SmartRow* add_row(DiskHealth* h, u8 id, const char* name) {
    if (h->row_count >= SMART_ROWS_MAX) return nullptr;
    SmartRow* r = &h->rows[h->row_count++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    snprintf(r->name, sizeof(r->name), "%s", name);
    return r;
}

// ---- NVMe --------------------------------------------------------------------------------------------

enum NvKind : u8 { NK_WARN_BITS, NK_KELVIN, NK_PCT, NK_DATA_UNITS, NK_COUNT, NK_MINUTES, NK_HOURS, NK_SECONDS };
struct NvField {
    u8 id;
    u16 offset;
    u8 len;
    NvKind kind;
    const char* name;
};
static const NvField NVME_FIELDS[] = {
    { 0x01, 0, 1, NK_WARN_BITS, "Critical Warning" },
    { 0x02, 1, 2, NK_KELVIN, "Composite Temperature" },
    { 0x03, 3, 1, NK_PCT, "Available Spare" },
    { 0x04, 4, 1, NK_PCT, "Available Spare Threshold" },
    { 0x05, 5, 1, NK_PCT, "Percentage Used" },
    { 0x06, 32, 16, NK_DATA_UNITS, "Data Units Read" },
    { 0x07, 48, 16, NK_DATA_UNITS, "Data Units Written" },
    { 0x08, 64, 16, NK_COUNT, "Host Read Commands" },
    { 0x09, 80, 16, NK_COUNT, "Host Write Commands" },
    { 0x0A, 96, 16, NK_MINUTES, "Controller Busy Time" },
    { 0x0B, 112, 16, NK_COUNT, "Power Cycles" },
    { 0x0C, 128, 16, NK_HOURS, "Power On Hours" },
    { 0x0D, 144, 16, NK_COUNT, "Unsafe Shutdowns" },
    { 0x0E, 160, 16, NK_COUNT, "Media and Data Integrity Errors" },
    { 0x0F, 176, 16, NK_COUNT, "Number of Error Information Log Entries" },
    { 0x10, 192, 4, NK_MINUTES, "Warning Composite Temperature Time" },
    { 0x11, 196, 4, NK_MINUTES, "Critical Composite Temperature Time" },
    { 0x12, 200, 2, NK_KELVIN, "Temperature Sensor 1" },
    { 0x13, 202, 2, NK_KELVIN, "Temperature Sensor 2" },
    { 0x14, 204, 2, NK_KELVIN, "Temperature Sensor 3" },
    { 0x15, 206, 2, NK_KELVIN, "Temperature Sensor 4" },
    { 0x16, 208, 2, NK_KELVIN, "Temperature Sensor 5" },
    { 0x17, 210, 2, NK_KELVIN, "Temperature Sensor 6" },
    { 0x18, 212, 2, NK_KELVIN, "Temperature Sensor 7" },
    { 0x19, 214, 2, NK_KELVIN, "Temperature Sensor 8" },
    { 0x1A, 216, 4, NK_COUNT, "Thermal Mgmt Temp 1 Transition Count" },
    { 0x1B, 220, 4, NK_COUNT, "Thermal Mgmt Temp 2 Transition Count" },
    { 0x1C, 224, 4, NK_SECONDS, "Total Time For Thermal Mgmt Temp 1" },
    { 0x1D, 228, 4, NK_SECONDS, "Total Time For Thermal Mgmt Temp 2" },
};

static void nvme_rows(DiskHealth* h) {
    const u8* log = h->nvme_health;
    for (u32 i = 0; i < ARRAY_COUNT(NVME_FIELDS); ++i) {
        const NvField* f = &NVME_FIELDS[i];
        const u8* p = log + f->offset;
        const u64 v = f->len == 1 ? p[0] : f->len == 2 ? le16(p) : f->len == 4 ? le32(p) : le64(p);
        if (f->kind == NK_KELVIN && f->id != 0x02 && v == 0) continue;     // sensor not implemented
        SmartRow* r = add_row(h, f->id, f->name);
        if (!r) return;
        r->raw_len = f->len > 6 ? 6 : f->len;
        memcpy(r->raw, p, f->len > 16 ? 16 : f->len);
        switch (f->kind) {
            case NK_WARN_BITS: snprintf(r->decoded, sizeof(r->decoded), v ? "0x%02llX" : "none", v); break;
            case NK_KELVIN: snprintf(r->decoded, sizeof(r->decoded), "%lld \xC2\xB0" "C", (long long)v - 273); break;
            case NK_PCT: snprintf(r->decoded, sizeof(r->decoded), "%llu %%", v); break;
            case NK_DATA_UNITS: bytes_text(v * 512000ull, r->decoded, sizeof(r->decoded)); break;
            case NK_COUNT: snprintf(r->decoded, sizeof(r->decoded), "%llu", v); break;
            case NK_MINUTES: snprintf(r->decoded, sizeof(r->decoded), "%llu min", v); break;
            case NK_HOURS: snprintf(r->decoded, sizeof(r->decoded), "%llu h", v); break;
            case NK_SECONDS: snprintf(r->decoded, sizeof(r->decoded), "%llu s", v); break;
        }
    }
}

static void nvme_selftest_parse(DiskHealth* h, const u8* log) {
    h->test_progress = -1;
    h->test_running[0] = 0;
    if (log[0] == 1 || log[0] == 2 || log[0] == 0xE) {
        h->test_progress = log[1] & 0x7F;
        snprintf(h->test_running, sizeof(h->test_running), "%s", log[0] == 1 ? "Short" : log[0] == 2 ? "Extended" : "Vendor");
    }
    static const char* const results[] = {
        "Completed without error", "Aborted by a Device Self-test command", "Aborted by a controller reset",
        "Aborted by a namespace removal", "Aborted by a Format NVM command", "Fatal or unknown test error",
        "Completed with an unknown segment failing", "Completed with one or more segments failing",
        "Aborted for an unknown reason", "Aborted by a sanitize operation",
    };
    h->test_count = 0;
    for (u32 i = 0; i < 20; ++i) {
        const u8* e = log + 4 + 28 * i;
        const u8 result = e[0] & 0xF, code = e[0] >> 4;
        if (result == 0xF) continue;    // unused entry
        SelfTestEntry* t = &h->tests[h->test_count++];
        memset(t, 0, sizeof(*t));
        snprintf(t->kind, sizeof(t->kind), "%s", code == 1 ? "Short" : code == 2 ? "Extended" : code == 0xE ? "Vendor specific" : "Unknown");
        if (result < ARRAY_COUNT(results)) snprintf(t->result, sizeof(t->result), "%s", results[result]);
        else snprintf(t->result, sizeof(t->result), "Result 0x%X", result);
        if (result >= 5 && result <= 7 && e[1]) cat(t->result, sizeof(t->result), " (segment %u)", e[1]);
        t->failed = result >= 5 && result <= 7;
        t->hours = le64(e + 4);
        t->lba_valid = (e[2] & 0x2) != 0;
        t->lba = le64(e + 16);
    }
}

static void read_nvme(DiskHealth* h) {
    NvmeDev* d = &h->nvme;
    snprintf(h->source, sizeof(h->source), "%s", NVME_TRANSPORT_NAMES[d->tr]);
    h->proto = HPROTO_NVME;
    h->rotation = 1;
    h->test_progress = -1;

    if ((h->have_nvme_id = nvme_identify(d, 1, 0, h->nvme_id))) {
        const u8* id = h->nvme_id;
        fixed_string(id + 24, 40, h->model, sizeof(h->model));
        fixed_string(id + 4, 20, h->serial, sizeof(h->serial));
        fixed_string(id + 64, 8, h->firmware, sizeof(h->firmware));
        h->nvme_version = le32(id + 80);
        const u32 maj = h->nvme_version >> 16, min = (h->nvme_version >> 8) & 0xFF, ter = h->nvme_version & 0xFF;
        if (maj) snprintf(h->standard, sizeof(h->standard), ter ? "NVM Express %u.%u.%u" : "NVM Express %u.%u", maj, min, ter);
        else snprintf(h->standard, sizeof(h->standard), "NVM Express 1.0 / 1.1");
        const u16 oacs = le16(id + 256), oncs = le16(id + 520);
        list_add(h->features, sizeof(h->features), "S.M.A.R.T.");
        if (oncs & 0x4) list_add(h->features, sizeof(h->features), "TRIM");
        if (id[525] & 0x1) list_add(h->features, sizeof(h->features), "VolatileWriteCache");
        if (id[265] & 0x1) list_add(h->features, sizeof(h->features), "APST");
        const u32 hmpre = le32(id + 272);
        if (hmpre) {
            char t[40];
            snprintf(t, sizeof(t), "HMB (%u MiB)", (u32)((u64)hmpre * 4096 / MiB));
            list_add(h->features, sizeof(h->features), t);
        }
        if (oacs & 0x10) list_add(h->features, sizeof(h->features), "Self-test");
        if (oacs & 0x2) list_add(h->features, sizeof(h->features), "Format NVM");
        if (le32(id + 328) & 0x7) list_add(h->features, sizeof(h->features), "Sanitize");
        if (oacs & 0x4) list_add(h->features, sizeof(h->features), "Firmware update");
        if (oacs & 0x1) list_add(h->features, sizeof(h->features), "Security");
        if (oncs & 0x8) list_add(h->features, sizeof(h->features), "Write Zeroes");
        if (le32(id + 516) > 1) list_add(h->features, sizeof(h->features), "Namespaces");
        h->can_self_test = h->can_short = h->can_extended = (oacs & 0x10) != 0;
        h->minutes_short = 2;          // the spec caps the short test at two minutes
        h->minutes_extended = le16(id + 316);
        const u16 wctemp = le16(id + 266), cctemp = le16(id + 268);
        if (wctemp > 273) h->warn_c = (f32)(wctemp - 273);
        if (cctemp > 273) h->crit_c = (f32)(cctemp - 273);
        h->power_state_count = id[263] + 1u;
        if (h->power_state_count > 32) h->power_state_count = 32;
        for (u32 i = 0; i < h->power_state_count; ++i) {
            const u8* ps = id + 2048 + 32 * i;
            NvmePowerState* p = &h->power_states[i];
            p->max_watts = le16(ps) * ((ps[3] & 1) ? 0.0001f : 0.01f);
            p->non_operational = (ps[3] & 2) != 0;
            p->entry_us = le32(ps + 4);
            p->exit_us = le32(ps + 8);
        }
    }
    if ((h->have_nvme_ns = nvme_identify(d, 0, 1, h->nvme_ns))) {
        const u8 flbas = h->nvme_ns[26] & 0xF;
        const u8 lbads = h->nvme_ns[128 + 4 * flbas + 2];
        if (lbads >= 9 && lbads < 16) h->logical_sector = 1u << lbads;
    }
    if ((h->have_nvme_health = nvme_get_log(d, 2, h->nvme_health, 512))) {
        const u8* l = h->nvme_health;
        h->have_power_on_hours = h->have_power_cycles = h->have_host_reads = h->have_host_writes = true;
        h->host_reads_bytes = le64(l + 32) * 512000ull;
        h->host_writes_bytes = le64(l + 48) * 512000ull;
        h->power_cycles = le64(l + 112);
        h->power_on_hours = le64(l + 128);
        h->media_errors = (i64)le64(l + 160);
        h->life_left_pct = l[5] >= 100 ? 0 : 100 - l[5];
        const u16 k = le16(l + 1);
        if (!h->have_temp && k > 200 && k < 400) { h->temp_c = (f32)(k - 273); h->have_temp = true; }
        nvme_rows(h);
    }
    if (h->have_nvme_id) {
        const u32 entries = h->nvme_id[262] + 1u;
        static u8 err[16 * 64];
        const u32 n = entries < 16 ? entries : 16;
        if (nvme_get_log(d, 1, err, n * 64)) {
            for (u32 i = 0; i < n; ++i) {
                const u8* e = err + 64 * i;
                const u64 count = le64(e);
                if (!count) continue;
                NvmeErrorEntry* x = &h->errors[h->error_count++];
                x->count = count;
                x->sqid = le16(e + 8);
                x->cmdid = le16(e + 10);
                x->status = le16(e + 12);
                x->lba = le64(e + 16);
                x->nsid = le32(e + 24);
            }
        }
    }
    h->have_nvme_fw = nvme_get_log(d, 3, h->nvme_fw, 512);
    static u8 st[564];
    if (nvme_get_log(d, 6, st, sizeof(st))) nvme_selftest_parse(h, st);
    static const u8 fids[] = { 0x01, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x10, 0x11 };
    for (u32 i = 0; i < ARRAY_COUNT(fids); ++i)
        if (nvme_get_feature(d, fids[i], &h->feature[fids[i]], nullptr, 0)) h->feature_ok |= 1u << fids[i];
    if (nvme_get_feature(d, 0x0C, &h->feature[0x0C], h->apst_table, sizeof(h->apst_table))) h->feature_ok |= 1u << 0x0C;

    // Interface / transfer mode.
    if (d->tr == NVME_TR_WINDOWS) {
        snprintf(h->interface_name, sizeof(h->interface_name), "NVM Express");
        if (h->loc.pcie_gen) snprintf(h->transfer_cur, sizeof(h->transfer_cur), "PCIe %u.0 x%u", h->loc.pcie_gen, h->loc.pcie_width);
        if (h->loc.pcie_gen_max)
            snprintf(h->transfer_max, sizeof(h->transfer_max), "PCIe %u.0 x%u", h->loc.pcie_gen_max, h->loc.pcie_width_max);
    } else {
        snprintf(h->interface_name, sizeof(h->interface_name), "USB (NVMe bridge %04X:%04X)", h->loc.usb_vid, h->loc.usb_pid);
    }
}

static void evaluate_nvme(DiskHealth* h) {
    const u8* l = h->nvme_health;
    const u8 cw = l[0], spare = l[3], spare_thr = l[4], used = l[5];
    h->status = HEALTH_GOOD;
    if (cw & 0x08) add_reason(h, HEALTH_BAD, "the drive has switched to read-only mode");
    if (cw & 0x04) add_reason(h, HEALTH_BAD, "the drive reports degraded reliability");
    if ((cw & 0x01) || spare < spare_thr) add_reason(h, HEALTH_BAD, "spare blocks (%u %%) are below the drive's threshold (%u %%)", spare, spare_thr);
    if (cw & 0x10) add_reason(h, HEALTH_BAD, "the cache backup (power-loss protection) failed");
    if (cw & 0x02) add_reason(h, HEALTH_CAUTION, "the drive is outside its temperature limits");
    if (h->media_errors >= CAUTION_COUNT)
        add_reason(h, HEALTH_CAUTION, "unrecovered media errors: %lld", (long long)h->media_errors);
    if (h->life_left_pct >= 0 && h->life_left_pct <= CAUTION_LIFE_LEFT_PCT)
        add_reason(h, HEALTH_CAUTION, "%d %% of rated endurance left (Caution at %d %%)", h->life_left_pct, CAUTION_LIFE_LEFT_PCT);
    for (u32 i = 0; i < h->row_count; ++i) {
        SmartRow* r = &h->rows[i];
        r->level = HEALTH_GOOD;
        if (r->id == 0x01 && cw) r->level = (cw & 0x1D) ? HEALTH_BAD : HEALTH_CAUTION;
        if (r->id == 0x03 && spare < spare_thr) r->level = HEALTH_BAD;
        if (r->id == 0x05 && 100 - (i32)used <= CAUTION_LIFE_LEFT_PCT) r->level = HEALTH_CAUTION;
        if (r->id == 0x0E && h->media_errors >= CAUTION_COUNT) r->level = HEALTH_CAUTION;
    }
}

// ---- ATA ---------------------------------------------------------------------------------------------

static void ata_selftest_parse(DiskHealth* h, const u8* log) {
    static const char* const results[] = {
        "Completed without error", "Aborted by host", "Interrupted by a reset", "Fatal error",
        "Completed with an unknown failure", "Completed with an electrical failure", "Completed with a servo failure",
        "Completed with a read failure", "Completed with handling damage",
    };
    h->test_count = 0;
    const u8 index = log[508];      // most recent entry, 1-based; 0 = empty log
    if (index == 0 || index > 21) return;
    for (u32 k = 0; k < 21; ++k) {
        const u32 i = (index - 1 + 21 - k) % 21;
        const u8* e = log + 2 + 24 * i;
        if (!e[0] && !e[1] && !e[2] && !e[3]) continue;
        SelfTestEntry* t = &h->tests[h->test_count++];
        memset(t, 0, sizeof(*t));
        const u8 type = e[0] & 0x7F;
        snprintf(t->kind, sizeof(t->kind), "%s%s", type == 0 ? "Off-line" : type == 1 ? "Short" : type == 2 ? "Extended" : type == 3 ? "Conveyance"
                                                   : type == 4 ? "Selective" : "Vendor specific",
                 (e[0] & 0x80) ? " (captive)" : "");
        const u8 r = e[1] >> 4;
        if (r == 0xF) snprintf(t->result, sizeof(t->result), "In progress (%u0 %% left)", e[1] & 0xF);
        else if (r < ARRAY_COUNT(results)) snprintf(t->result, sizeof(t->result), "%s", results[r]);
        else snprintf(t->result, sizeof(t->result), "Result 0x%X", r);
        t->failed = r >= 3 && r <= 8;
        t->hours = le16(e + 2);
        t->lba = le32(e + 5);
        t->lba_valid = t->failed && t->lba != 0xFFFFFFFF;
    }
}

static void ata_progress(DiskHealth* h) {
    h->test_progress = -1;
    h->test_running[0] = 0;
    if ((h->caps.selftest_status >> 4) == 0xF) {
        h->test_progress = 100 - (h->caps.selftest_status & 0xF) * 10;
        snprintf(h->test_running, sizeof(h->test_running), "Self-test");
    }
}

static void ata_rows(DiskHealth* h) {
    const u8* s = h->smart_data;
    for (u32 i = 0; i < 30; ++i) {
        const u8* a = s + 2 + 12 * i;
        if (!a[0]) continue;
        const SmartAttrDef* def = smart_attr_def(h->vendor, a[0]);
        SmartRow* r = add_row(h, a[0], def->name);
        if (!r) return;
        r->has_values = true;
        r->flags = le16(a + 1);
        r->current = a[3];
        r->worst = a[4];
        memcpy(r->raw, a + 5, 7);
        r->raw_len = 6;
        r->unit = def->unit;
        if (h->have_thresholds)
            for (u32 k = 0; k < 30; ++k)
                if (h->smart_thresholds[2 + 12 * k] == a[0]) r->threshold = h->smart_thresholds[3 + 12 * k];
        smart_decode(def->unit, r->raw, r->current, r->decoded, sizeof(r->decoded));
        const u64 raw = smart_raw48(r->raw);
        const u32 raw32 = (u32)raw;
        switch (a[0]) {
            case 0x05: h->reallocated = raw32; break;
            case 0xC5: h->pending = raw32; break;
            case 0xC6: h->uncorrectable = raw32; break;
            case 0x09:
                if (def->unit == SU_HOURS) { h->power_on_hours = raw32; h->have_power_on_hours = true; }
                break;
            case 0x0C: h->power_cycles = raw32; h->have_power_cycles = true; break;
            case 0xC2:
            case 0xBE:
                if (!h->have_temp && r->raw[0] > 0 && r->raw[0] < 100) { h->temp_c = r->raw[0]; h->have_temp = true; }
                break;
        }
        const u64 bytes = smart_bytes(def->unit, r->raw);
        if (bytes && !h->have_host_writes && (a[0] == 0xF1 || a[0] == 0xF6 || a[0] == 0xE1)) {
            h->host_writes_bytes = bytes;
            h->have_host_writes = true;
        }
        if (bytes && !h->have_host_reads && a[0] == 0xF2) { h->host_reads_bytes = bytes; h->have_host_reads = true; }
        if (bytes && !h->have_nand_writes && (a[0] == 0xF9 || (a[0] == 0xE9 && def->unit == SU_GIB))) {
            h->nand_writes_bytes = bytes;
            h->have_nand_writes = true;
        }
        if (def->unit == SU_LIFE && h->life_left_pct < 0 && r->current <= 100) h->life_left_pct = r->current;
    }
}

// Device Statistics log (GPL 0x04) through the query path: no administrator rights needed.
static void ata_device_statistics(DiskHealth* h, HANDLE q) {
    u8 page[512];
    auto stat = [&](u32 off, u64* out) {
        const u64 v = le64(page + off);
        if ((v >> 62) != 3) return false;      // supported + valid
        *out = v & 0xFFFFFFFFFFFFull;
        return true;
    };
    u64 v = 0;
    if (ata_query_log(q, 0x04, 1, page)) {
        if (!h->have_power_cycles && stat(8, &v)) { h->power_cycles = v; h->have_power_cycles = true; }
        if (!h->have_power_on_hours && stat(16, &v)) { h->power_on_hours = v; h->have_power_on_hours = true; }
        const u32 ls = h->id.logical_sector ? h->id.logical_sector : 512;
        if (!h->have_host_writes && stat(24, &v)) { h->host_writes_bytes = v * ls; h->have_host_writes = true; }
        if (!h->have_host_reads && stat(40, &v)) { h->host_reads_bytes = v * ls; h->have_host_reads = true; }
    }
    if (ata_query_log(q, 0x04, 4, page) && h->uncorrectable < 0 && stat(8, &v)) h->uncorrectable = (i64)v;
    if (ata_query_log(q, 0x04, 5, page) && !h->have_temp && stat(8, &v) && (i8)v > -60) { h->temp_c = (i8)v; h->have_temp = true; }
    if (ata_query_log(q, 0x04, 7, page) && h->life_left_pct < 0 && stat(8, &v) && (v & 0xFF) <= 255)
        h->life_left_pct = (v & 0xFF) >= 100 ? 0 : 100 - (i32)(v & 0xFF);
}

static void ata_info(DiskHealth* h) {
    const AtaIdInfo* id = &h->id;
    snprintf(h->model, sizeof(h->model), "%s", id->model);
    snprintf(h->serial, sizeof(h->serial), "%s", id->serial);
    snprintf(h->firmware, sizeof(h->firmware), "%s", id->firmware);
    snprintf(h->standard, sizeof(h->standard), "%s", id->standard);
    h->rotation = id->rotation;
    h->buffer_bytes = id->buffer_bytes;
    h->logical_sector = id->logical_sector;
    h->physical_sector = id->physical_sector;
    static const char* const sata_rate[] = { "", "SATA/150", "SATA/300", "SATA/600" };
    if (h->disk.bus_type == BusTypeUsb || h->loc.usb_vid)
        snprintf(h->interface_name, sizeof(h->interface_name), "USB (SATA bridge %04X:%04X)", h->loc.usb_vid, h->loc.usb_pid);
    else if (h->raid_member)
        snprintf(h->interface_name, sizeof(h->interface_name), "RAID member (CSMI)");
    else
        snprintf(h->interface_name, sizeof(h->interface_name), "%s", id->sata_gen_max ? "Serial ATA" : "ATA");
    if (id->sata_gen_cur && id->sata_gen_cur <= 3) snprintf(h->transfer_cur, sizeof(h->transfer_cur), "%s", sata_rate[id->sata_gen_cur]);
    if (id->sata_gen_max && id->sata_gen_max <= 3) snprintf(h->transfer_max, sizeof(h->transfer_max), "%s", sata_rate[id->sata_gen_max]);
    char* f = h->features;
    const u32 fs = sizeof(h->features);
    if (id->smart) list_add(f, fs, id->smart_enabled ? "S.M.A.R.T." : "S.M.A.R.T. (disabled)");
    if (id->lba48) list_add(f, fs, "48bit LBA");
    if (id->ncq) list_add(f, fs, "NCQ");
    if (id->trim) list_add(f, fs, "TRIM");
    if (id->write_cache) list_add(f, fs, id->write_cache_enabled ? "VolatileWriteCache" : "VolatileWriteCache (off)");
    if (id->devsleep) list_add(f, fs, "DevSleep");
    if (id->apm) list_add(f, fs, id->apm_enabled ? "APM" : "APM (off)");
    if (id->aam) list_add(f, fs, id->aam_enabled ? "AAM" : "AAM (off)");
    if (id->gpl) list_add(f, fs, "GPL");
    if (id->sct) list_add(f, fs, "SCT");
    if (id->security) list_add(f, fs, "Security");
    if (id->sanitize) list_add(f, fs, "Sanitize");
    h->vendor = smart_vendor(id->model, id->rotation == 1);
}

static void read_ata_smart(DiskHealth* h) {
    AtaDev* d = &h->ata;
    if (!h->id.smart) return;
    h->have_smart = ata_smart_read_data(d, h->smart_data);
    h->have_thresholds = h->have_smart && ata_smart_read_thresholds(d, h->smart_thresholds);
    h->smart_status = ata_smart_status(d);
    if (!h->have_smart) return;
    ata_parse_smart_caps(h->smart_data, &h->caps);
    h->can_self_test = h->can_short = h->can_extended = h->caps.can_selftest;
    h->can_conveyance = h->caps.can_conveyance;
    h->minutes_short = h->caps.poll_short_min;
    h->minutes_extended = h->caps.poll_extended_min;
    h->minutes_conveyance = h->caps.poll_conveyance_min;
    ata_progress(h);
    u8 log[512];
    if (h->caps.can_selftest && ata_smart_read_log(d, 0x06, log)) ata_selftest_parse(h, log);
    if (h->caps.error_log && ata_smart_read_log(d, 0x01, log)) h->ata_error_count = le16(log + 452);
}

static void read_ata(DiskHealth* h, HANDLE q, HANDLE rw) {
    h->proto = HPROTO_ATA;
    h->test_progress = -1;
    if (rw != INVALID_HANDLE_VALUE && ata_attach(&h->ata, rw, h->disk.bus_type, h->loc.usb_vid, h->loc.usb_pid, h->ata_id)) {
        h->have_ata_id = true;
        ata_parse_identify(h->ata_id, &h->id);
        ata_info(h);
        snprintf(h->source, sizeof(h->source), "%s", ATA_TRANSPORT_NAMES[h->ata.tr]);
        read_ata_smart(h);
        ata_rows(h);
        h->ata.h = nullptr;     // the handle is closed after the scan; reopened for self-tests
        return;
    }
    // Without administrator rights: what the storage driver hands out on a query-only handle.
    if (ata_query_identify(q, h->ata_id)) {
        h->have_ata_id = true;
        ata_parse_identify(h->ata_id, &h->id);
        ata_info(h);
        snprintf(h->source, sizeof(h->source), "Windows storage query (IDENTIFY, device statistics)");
        if (h->id.smart && ata_query_smart(q, 0xD0, h->smart_data)) {
            h->have_smart = true;
            h->have_thresholds = ata_query_smart(q, 0xD1, h->smart_thresholds);
            ata_parse_smart_caps(h->smart_data, &h->caps);
            ata_progress(h);
            snprintf(h->source, sizeof(h->source), "SMART_RCV_DRIVE_DATA_EX (query handle)");
            ata_rows(h);
        }
        ata_device_statistics(h, q);
    }
}

static void evaluate_ata(DiskHealth* h) {
    h->status = HEALTH_GOOD;
    if (h->smart_status == 1) add_reason(h, HEALTH_BAD, "SMART reports that the drive is failing (threshold exceeded)");
    for (u32 i = 0; i < h->row_count; ++i) {
        SmartRow* r = &h->rows[i];
        r->level = HEALTH_GOOD;
        if (r->threshold && r->threshold < 0xFE && r->current && r->current <= r->threshold) {
            r->level = HEALTH_BAD;
            add_reason(h, HEALTH_BAD, "%02X %s is at or below its threshold (%u <= %u)", r->id, r->name, r->current, r->threshold);
        }
        const bool counter = r->id == 0x05 || r->id == 0xC5 || r->id == 0xC6;
        if (counter && (u32)smart_raw48(r->raw) >= CAUTION_COUNT && r->level < HEALTH_CAUTION) r->level = HEALTH_CAUTION;
        if (r->unit == SU_LIFE && r->current <= CAUTION_LIFE_LEFT_PCT && r->level < HEALTH_CAUTION) r->level = HEALTH_CAUTION;
    }
    if (h->reallocated >= CAUTION_COUNT)
        add_reason(h, HEALTH_CAUTION, "reallocated sectors: %lld", (long long)h->reallocated);
    if (h->pending >= CAUTION_COUNT)
        add_reason(h, HEALTH_CAUTION, "sectors pending reallocation: %lld", (long long)h->pending);
    if (h->uncorrectable >= CAUTION_COUNT)
        add_reason(h, HEALTH_CAUTION, "uncorrectable sectors: %lld", (long long)h->uncorrectable);
    if (h->life_left_pct >= 0 && h->life_left_pct <= CAUTION_LIFE_LEFT_PCT)
        add_reason(h, HEALTH_CAUTION, "%d %% of rated endurance left (Caution at %d %%)", h->life_left_pct, CAUTION_LIFE_LEFT_PCT);
    if (!h->have_smart && h->uncorrectable < 0 && h->life_left_pct < 0) {
        h->status = HEALTH_UNKNOWN;
        snprintf(h->status_text, sizeof(h->status_text), "%s",
                 h->full_access ? "The drive answered IDENTIFY but not the SMART commands."
                                : "Run EzPcBench as administrator to read this drive's SMART attributes.");
    }
}

// ---- per drive --------------------------------------------------------------------------------------

// Status and reasons from the data read. Drives without data keep the message read_disk left.
static void evaluate(DiskHealth* h) {
    if (h->proto == HPROTO_NVME && h->have_nvme_health) {
        h->status_text[0] = 0;
        evaluate_nvme(h);
    } else if (h->proto == HPROTO_ATA && h->have_ata_id) {
        h->status_text[0] = 0;
        evaluate_ata(h);
    } else if (h->have_predict) {
        h->status = h->predict_failure ? HEALTH_BAD : HEALTH_GOOD;
        snprintf(h->status_text, sizeof(h->status_text), "%s", h->predict_failure
                     ? "SMART predicts that this drive will fail. Back up its data."
                     : "SMART reports no predicted failure (detailed attributes aren't available for this drive).");
        return;
    }
    if (h->status == HEALTH_GOOD && !h->status_text[0])
        snprintf(h->status_text, sizeof(h->status_text), "No problems reported by the drive.");
}

static void finish_info(DiskHealth* h, HANDLE q) {
    if (!h->model[0]) snprintf(h->model, sizeof(h->model), "%s", h->disk.model);
    if (!h->serial[0]) snprintf(h->serial, sizeof(h->serial), "%s", h->disk.serial);
    if (!h->firmware[0]) snprintf(h->firmware, sizeof(h->firmware), "%s", h->disk.firmware);
    if (!h->interface_name[0]) snprintf(h->interface_name, sizeof(h->interface_name), "%s", h->disk.bus);
    if (q == INVALID_HANDLE_VALUE) return;
    i32 trim, wc, seek;
    physdisk_flags(q, &trim, &wc, &seek);
    if (!h->rotation && seek == 0) h->rotation = 1;
    if (trim == 1 && !strstr(h->features, "TRIM")) list_add(h->features, sizeof(h->features), "TRIM");
    if (wc == 1 && !strstr(h->features, "VolatileWriteCache")) list_add(h->features, sizeof(h->features), "VolatileWriteCache");
    if (!h->transfer_cur[0] && h->proto != HPROTO_NVME) {
        u16 maj = 0, min = 0;
        bool queueing = false;
        if (physdisk_adapter(q, &maj, &min, &queueing) && queueing && !strstr(h->features, "NCQ") && h->disk.bus_type == BusTypeSata)
            list_add(h->features, sizeof(h->features), "NCQ");
    }
}

static void read_disk(DiskHealth* h) {
    h->status = HEALTH_UNKNOWN;
    h->smart_status = -1;
    h->life_left_pct = -1;
    h->reallocated = h->pending = h->uncorrectable = h->media_errors = -1;
    h->test_progress = -1;
    if (h->disk.virtual_disk) {
        snprintf(h->status_text, sizeof(h->status_text), "Virtual disk: its health is that of the drive holding its file.");
        snprintf(h->interface_name, sizeof(h->interface_name), "%s", h->disk.bus);
        snprintf(h->model, sizeof(h->model), "%s", h->disk.model);
        HANDLE q = physdisk_open(h->disk.number, false);
        if (q != INVALID_HANDLE_VALUE) {
            h->part_count = physdisk_partitions(q, h->disk.number, h->parts, PARTITIONS_MAX, h->part_style);
            CloseHandle(q);
        }
        return;
    }
    HANDLE q = physdisk_open(h->disk.number, false);
    HANDLE rw = physdisk_open(h->disk.number, true);
    h->full_access = rw != INVALID_HANDLE_VALUE;
    if (q == INVALID_HANDLE_VALUE && rw == INVALID_HANDLE_VALUE) {
        snprintf(h->status_text, sizeof(h->status_text), "Cannot open the drive (error %lu).", GetLastError());
        return;
    }
    if (q == INVALID_HANDLE_VALUE) q = rw;
    physdisk_location(h->disk.number, &h->loc);
    h->have_temp = physdisk_temperature(q, &h->temp_c, &h->warn_c);
    h->part_count = physdisk_partitions(q, h->disk.number, h->parts, PARTITIONS_MAX, h->part_style);

    const NvmeTransport bridge = nvme_bridge_kind(h->loc.usb_vid, h->loc.usb_pid);
    h->nvme.h = q;
    h->nvme.rw = nullptr;
    if (h->disk.bus_type == BusTypeNvme) {
        h->nvme.tr = NVME_TR_WINDOWS;
        read_nvme(h);
    } else if (bridge != NVME_TR_NONE && h->full_access && nvme_attach_bridge(&h->nvme, rw, bridge, h->nvme_id)) {
        read_nvme(h);
    } else {
        read_ata(h, q, rw);
    }
    h->nvme.h = h->nvme.rw = nullptr;     // handles close below

    if (h->proto == HPROTO_NONE || (!h->have_nvme_health && !h->have_ata_id)) {
        STORAGE_PREDICT_FAILURE pf = {};
        DWORD got = 0;
        if (DeviceIoControl(rw != INVALID_HANDLE_VALUE ? rw : q, IOCTL_STORAGE_PREDICT_FAILURE, nullptr, 0, &pf, sizeof(pf), &got, nullptr)) {
            h->have_predict = true;
            h->predict_failure = pf.PredictFailure != 0;
            snprintf(h->source, sizeof(h->source), "IOCTL_STORAGE_PREDICT_FAILURE");
        } else if (!h->full_access) {
            snprintf(h->status_text, sizeof(h->status_text), "Run EzPcBench as administrator to read this drive's health data.");
        } else {
            snprintf(h->status_text, sizeof(h->status_text), "This drive or its driver doesn't report health data to Windows.");
        }
    }
    finish_info(h, q);
    if (rw != INVALID_HANDLE_VALUE && rw != q) CloseHandle(rw);
    CloseHandle(q);
}

static void read_raid_member(DiskHealth* h, CsmiDisk* c) {
    h->raid_member = true;
    h->status = HEALTH_UNKNOWN;
    h->smart_status = -1;
    h->life_left_pct = -1;
    h->reallocated = h->pending = h->uncorrectable = h->media_errors = -1;
    h->test_progress = -1;
    h->full_access = true;
    h->proto = HPROTO_ATA;
    h->ata = c->dev;
    memcpy(h->ata_id, c->id, 512);
    h->have_ata_id = true;
    ata_parse_identify(h->ata_id, &h->id);
    ata_info(h);
    snprintf(h->disk.model, sizeof(h->disk.model), "%s", h->id.model);
    snprintf(h->disk.bus, sizeof(h->disk.bus), "RAID");
    h->disk.size_bytes = h->id.sectors * (h->id.logical_sector ? h->id.logical_sector : 512);
    snprintf(h->source, sizeof(h->source), "CSMI STP pass-through (\\\\.\\Scsi%u:, phy %u)", c->scsi_port, c->dev.port);
    read_ata_smart(h);
    ata_rows(h);
}

u32 disk_health_scan(DiskHealth* out, u32 max) {
    PhysDisk disks[PHYSDISK_MAX];
    u32 n = physdisk_enum(disks, PHYSDISK_MAX < max ? PHYSDISK_MAX : max);
    bool raid = false;
    for (u32 i = 0; i < n; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].disk = disks[i];
        read_disk(&out[i]);
        raid |= disks[i].bus_type == BusTypeRAID;
    }
    // RAID volumes hide their members; reach SATA members through CSMI (administrator only).
    disk_health_close();
    if (raid) {
        s_csmi_count = csmi_scan(s_csmi, CSMI_MAX);
        for (u32 i = 0; i < s_csmi_count && n < max; ++i) {
            memset(&out[n], 0, sizeof(out[n]));
            out[n].disk.number = 1000 + i;
            out[n].csmi_index = i;
            read_raid_member(&out[n], &s_csmi[i]);
            ++n;
        }
    }
    for (u32 i = 0; i < n; ++i)
        if (!out[i].disk.virtual_disk) evaluate(&out[i]);
    return n;
}

void disk_health_close() {
    csmi_close(s_csmi, s_csmi_count);
    s_csmi_count = 0;
}

// ---- self-tests -------------------------------------------------------------------------------------------

// Read/write handle for commands: the RAID controller handle, or the disk reopened.
static HANDLE command_handle(DiskHealth* h, bool* close) {
    *close = false;
    if (h->raid_member) return h->csmi_index < s_csmi_count ? s_csmi[h->csmi_index].dev.h : INVALID_HANDLE_VALUE;
    HANDLE rw = physdisk_open(h->disk.number, true);
    *close = rw != INVALID_HANDLE_VALUE;
    return rw;
}

bool disk_selftest_start(DiskHealth* h, SelfTestKind kind, char* err, u32 err_size) {
    err[0] = 0;
    bool close = false;
    HANDLE rw = command_handle(h, &close);
    bool ok = false;
    if (h->proto == HPROTO_NVME) {
        static const u8 codes[] = { 1, 2, 0, 0xF };
        if (kind == SELFTEST_CONVEYANCE) {
            snprintf(err, err_size, "NVMe has no conveyance test");
        } else {
            NvmeDev d = h->nvme;
            d.h = physdisk_open(h->disk.number, false);
            d.rw = rw;
            ok = nvme_self_test(&d, codes[kind], err, err_size);
            if (d.h != INVALID_HANDLE_VALUE) CloseHandle(d.h);
        }
    } else if (h->proto == HPROTO_ATA && h->ata.tr != ATA_TR_NONE) {
        if (rw == INVALID_HANDLE_VALUE) {
            snprintf(err, err_size, "needs administrator rights");
        } else {
            static const u8 sub[] = { 1, 2, 3, 127 };
            AtaDev d = h->ata;
            d.h = rw;
            ok = ata_smart_execute(&d, sub[kind]);
            if (!ok) snprintf(err, err_size, "the drive rejected the command");
        }
    } else {
        snprintf(err, err_size, h->full_access ? "self-tests aren't reachable on this drive" : "needs administrator rights");
    }
    if (close) CloseHandle(rw);
    if (ok) disk_selftest_poll(h);
    return ok;
}

void disk_selftest_poll(DiskHealth* h) {
    if (h->proto == HPROTO_NVME) {
        NvmeDev d = h->nvme;
        bool close = false;
        d.h = physdisk_open(h->disk.number, false);
        if (d.tr != NVME_TR_WINDOWS) d.rw = command_handle(h, &close);
        static u8 st[564];
        if (nvme_get_log(&d, 6, st, sizeof(st))) nvme_selftest_parse(h, st);
        if (d.h != INVALID_HANDLE_VALUE) CloseHandle(d.h);
        if (close) CloseHandle(d.rw);
    } else if (h->proto == HPROTO_ATA && h->ata.tr != ATA_TR_NONE) {
        bool close = false;
        HANDLE rw = command_handle(h, &close);
        if (rw == INVALID_HANDLE_VALUE) return;
        AtaDev d = h->ata;
        d.h = rw;
        if (ata_smart_read_data(&d, h->smart_data)) {
            ata_parse_smart_caps(h->smart_data, &h->caps);
            ata_progress(h);
            u8 log[512];
            if (ata_smart_read_log(&d, 0x06, log)) ata_selftest_parse(h, log);
        }
        if (close) CloseHandle(rw);
    }
}

// ---- text report ------------------------------------------------------------------------------------------

struct Text {
    char* p;
    u32 len, cap;
};

static void out(Text* t, const char* fmt, ...) {
    if (t->len + 1 >= t->cap) return;
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(t->p + t->len, t->cap - t->len, fmt, a);
    va_end(a);
    if (n > 0) t->len = t->len + (u32)n < t->cap ? t->len + (u32)n : t->cap - 1;
}

static void line(Text* t, char c) {
    char l[81];
    memset(l, c, 80);
    l[80] = 0;
    out(t, "%s\r\n", l);
}

static void field(Text* t, const char* name, const char* value) {
    if (value && value[0]) out(t, "%18s : %s\r\n", name, value);
}

u32 disk_health_report(const DiskHealth* drives, u32 count, bool hide_serial, char* buf, u32 size) {
    Text t = { buf, 0, size };
    buf[0] = 0;
    const time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
    out(&t, "EzPcBench disk health report (%s)\r\n\r\n", date);
    for (u32 i = 0; i < count; ++i) {
        const DiskHealth* h = &drives[i];
        char v[160], a[64], b[64];
        line(&t, '-');
        out(&t, " (%u) %s\r\n", i + 1, h->model[0] ? h->model : h->disk.model);
        line(&t, '-');
        field(&t, "Model", h->model);
        field(&t, "Firmware", h->firmware);
        field(&t, "Serial number", hide_serial && h->serial[0] ? "********" : h->serial);
        snprintf(v, sizeof(v), "%.1f GB", h->disk.size_bytes / 1e9);
        field(&t, "Disk size", v);
        field(&t, "Interface", h->interface_name);
        if (h->transfer_cur[0] || h->transfer_max[0]) {
            snprintf(v, sizeof(v), "%s | %s", h->transfer_cur[0] ? h->transfer_cur : "----", h->transfer_max[0] ? h->transfer_max : "----");
            field(&t, "Transfer mode", v);
        }
        field(&t, "Standard", h->standard);
        field(&t, "Features", h->features);
        if (h->rotation) {
            if (h->rotation == 1) snprintf(v, sizeof(v), "---- (SSD)");
            else snprintf(v, sizeof(v), "%u RPM", h->rotation);
            field(&t, "Rotation rate", v);
        }
        if (h->buffer_bytes) { snprintf(v, sizeof(v), "%u KB", (u32)(h->buffer_bytes / 1024)); field(&t, "Buffer size", v); }
        if (h->have_power_cycles) { snprintf(v, sizeof(v), "%llu", h->power_cycles); field(&t, "Power on count", v); }
        if (h->have_power_on_hours) { snprintf(v, sizeof(v), "%llu hours", h->power_on_hours); field(&t, "Power on hours", v); }
        if (h->have_host_reads) { bytes_text(h->host_reads_bytes, v, sizeof(v)); field(&t, "Host reads", v); }
        if (h->have_host_writes) { bytes_text(h->host_writes_bytes, v, sizeof(v)); field(&t, "Host writes", v); }
        if (h->have_nand_writes) { bytes_text(h->nand_writes_bytes, v, sizeof(v)); field(&t, "NAND writes", v); }
        if (h->have_temp) { snprintf(v, sizeof(v), "%.0f C", h->temp_c); field(&t, "Temperature", v); }
        if (h->life_left_pct >= 0) snprintf(v, sizeof(v), "%s (%d %% life left)", HEALTH_STATUS_NAMES[h->status], h->life_left_pct);
        else snprintf(v, sizeof(v), "%s", HEALTH_STATUS_NAMES[h->status]);
        field(&t, "Health status", v);
        field(&t, "Details", h->status_text);
        field(&t, "Drive letter", h->disk.letters);
        for (u32 p = 0; p < h->part_count; ++p) {
            const DiskPartition* pt = &h->parts[p];
            a[0] = 0;
            if (p == 0) snprintf(a, sizeof(a), "Partitions (%s)", h->part_style);
            snprintf(v, sizeof(v), "#%u %s, %.1f GB", pt->number, pt->type, pt->size / 1e9);
            if (pt->letter) cat(v, sizeof(v), ", %c:", pt->letter);
            if (pt->fs[0]) cat(v, sizeof(v), ", %s", pt->fs);
            if (pt->label[0]) cat(v, sizeof(v), " \"%s\"", pt->label);
            out(&t, "%18s : %s\r\n", a, v);
        }
        field(&t, "Read via", h->source);

        if (h->row_count) {
            out(&t, "\r\n-- S.M.A.R.T. %s\r\n", h->proto == HPROTO_NVME ? "(NVMe health log)" : "");
            if (h->proto == HPROTO_ATA) out(&t, "ID Cur Wor Thr RawValues(6)  Attribute Name                          Decoded\r\n");
            else out(&t, "ID RawValues(6)  Attribute Name                          Decoded\r\n");
            for (u32 r = 0; r < h->row_count; ++r) {
                const SmartRow* row = &h->rows[r];
                char raw[32] = {};
                for (u32 k = 0; k < 6; ++k) {      // most significant byte first, like CrystalDiskInfo
                    const u32 idx = 5 - k;
                    snprintf(raw + 2 * k, 3, "%02X", idx < row->raw_len ? row->raw[idx] : 0);
                }
                if (h->proto == HPROTO_ATA)
                    out(&t, "%02X %3u %3u %3u %s  %-38s  %s%s\r\n", row->id, row->current, row->worst, row->threshold, raw, row->name,
                        row->decoded, row->level == HEALTH_BAD ? "  [BAD]" : row->level == HEALTH_CAUTION ? "  [CAUTION]" : "");
                else
                    out(&t, "%02X %s  %-38s  %s%s\r\n", row->id, raw, row->name, row->decoded,
                        row->level == HEALTH_BAD ? "  [BAD]" : row->level == HEALTH_CAUTION ? "  [CAUTION]" : "");
            }
            if (h->smart_status >= 0) out(&t, "SMART RETURN STATUS: %s\r\n", h->smart_status ? "threshold exceeded" : "OK");
            if (h->ata_error_count) out(&t, "ATA error log: %u errors\r\n", h->ata_error_count);
        }
        if (h->proto == HPROTO_NVME && h->have_nvme_id) {
            const u8* id = h->nvme_id;
            out(&t, "\r\n-- NVMe controller\r\n");
            snprintf(v, sizeof(v), "%04X / %04X", le16(id), le16(id + 2));
            field(&t, "PCI vendor / subsys", v);
            snprintf(v, sizeof(v), "%02X%02X%02X", id[75], id[74], id[73]);
            field(&t, "IEEE OUI", v);
            snprintf(v, sizeof(v), "%u", le32(id + 516));
            field(&t, "Namespaces", v);
            if (h->logical_sector) { snprintf(v, sizeof(v), "%u bytes", h->logical_sector); field(&t, "LBA size", v); }
            snprintf(v, sizeof(v), "%.0f C / %.0f C", h->warn_c, h->crit_c);
            field(&t, "Warning / critical", v);
            if (h->minutes_extended) { snprintf(v, sizeof(v), "%u min", h->minutes_extended); field(&t, "Extended self-test", v); }
            for (u32 p = 0; p < h->power_state_count; ++p) {
                const NvmePowerState* ps = &h->power_states[p];
                snprintf(a, sizeof(a), "Power state %u", p);
                snprintf(v, sizeof(v), ps->max_watts >= 0.1f ? "%.2f W%s, entry %u us, exit %u us" : "%.4f W%s, entry %u us, exit %u us",
                         ps->max_watts, ps->non_operational ? " (non-operational)" : "", ps->entry_us, ps->exit_us);
                field(&t, a, v);
            }
            if (h->feature_ok & (1u << 0x06)) field(&t, "Write cache", (h->feature[0x06] & 1) ? "enabled" : "disabled");
            if (h->feature_ok & (1u << 0x0C)) field(&t, "APST", (h->feature[0x0C] & 1) ? "enabled" : "disabled");
            if (h->feature_ok & (1u << 0x02)) { snprintf(v, sizeof(v), "PS%u", h->feature[0x02] & 0x1F); field(&t, "Power state now", v); }
            if (h->feature_ok & (1u << 0x10)) {
                const u32 f = h->feature[0x10];
                if (f) {
                    snprintf(v, sizeof(v), "TMT1 %d C, TMT2 %d C", (i32)(f >> 16) - 273, (i32)(f & 0xFFFF) - 273);
                    field(&t, "Host thermal mgmt", v);
                }
            }
            if (h->have_nvme_fw) {
                const u8* fw = h->nvme_fw;
                const u32 slots = (id[260] >> 1) & 7;
                for (u32 s = 0; s < slots && s < 7; ++s) {
                    char rev[9];
                    fixed_string(fw + 8 + 8 * s, 8, rev, sizeof(rev));
                    if (!rev[0]) continue;
                    snprintf(a, sizeof(a), "Firmware slot %u", s + 1);
                    snprintf(b, sizeof(b), "%s%s%s", rev, (fw[0] & 7) == s + 1 ? " (active)" : "", s == 0 && (id[260] & 1) ? " (read-only)" : "");
                    field(&t, a, b);
                }
            }
            if (h->error_count) {
                out(&t, "\r\n-- NVMe error log (newest first)\r\n");
                for (u32 e = 0; e < h->error_count; ++e) {
                    const NvmeErrorEntry* x = &h->errors[e];
                    out(&t, "#%llu  SQ %u  CID %u  status 0x%04X  NSID %u  LBA %llu\r\n", x->count, x->sqid, x->cmdid, x->status, x->nsid, x->lba);
                }
            }
        }
        if (h->test_count || h->test_progress >= 0 || h->can_self_test) {
            out(&t, "\r\n-- Self-test log (newest first)\r\n");
            if (!h->test_count && h->test_progress < 0) out(&t, "No self-tests recorded.\r\n");
            if (h->test_progress >= 0) out(&t, "%s test running: %d %%\r\n", h->test_running, h->test_progress);
            for (u32 k = 0; k < h->test_count; ++k) {
                const SelfTestEntry* e = &h->tests[k];
                out(&t, "%-20s %-48s %8llu h", e->kind, e->result, e->hours);
                if (e->lba_valid) out(&t, "  LBA %llu", e->lba);
                out(&t, "\r\n");
            }
        }
        out(&t, "\r\n");
    }
    return t.len;
}
