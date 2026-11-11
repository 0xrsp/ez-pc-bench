// ATA command transports (see ata.h). CDB layouts of the USB bridges follow smartmontools (scsiata.cpp).
#include "core/ata.h"
#include "core/csmi.h"
#include "core/scsi_pt.h"
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef SMART_RCV_DRIVE_DATA_EX
#define SMART_RCV_DRIVE_DATA_EX CTL_CODE(IOCTL_DISK_BASE, 0x0023, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

const char* const ATA_TRANSPORT_NAMES[ATA_TR_COUNT] = {
    "none", "ATA pass-through", "SMART IOCTL", "SAT (ATA PASS-THROUGH 16)", "SAT (ATA PASS-THROUGH 12)",
    "JMicron USB bridge", "Prolific USB bridge", "Sunplus USB bridge", "Cypress USB bridge", "CSMI (RAID)",
};

constexpr u8 ATA_CMD_IDENTIFY = 0xEC, ATA_CMD_SMART = 0xB0, ATA_CMD_SET_FEATURES = 0xEF;
constexpr u8 SUB_READ_DATA = 0xD0, SUB_READ_THRESHOLDS = 0xD1, SUB_EXECUTE_OFFLINE = 0xD4, SUB_READ_LOG = 0xD5,
             SUB_RETURN_STATUS = 0xDA;

// ---- transports -----------------------------------------------------------------------------------

static bool ata_pt(AtaDev* d, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    struct Buf {
        ATA_PASS_THROUGH_EX apt;
        ULONG filler;
        u8 data[512 * 2];
    };
    Buf b = {};
    const u32 size = sectors * 512;
    if (size > sizeof(b.data)) return false;
    b.apt.Length = sizeof(ATA_PASS_THROUGH_EX);
    b.apt.AtaFlags = ATA_FLAGS_DRDY_REQUIRED | (dir == ATA_DATA_IN ? ATA_FLAGS_DATA_IN : dir == ATA_DATA_OUT ? ATA_FLAGS_DATA_OUT : 0);
    b.apt.DataTransferLength = size;
    b.apt.TimeOutValue = 15;
    b.apt.DataBufferOffset = offsetof(Buf, data);
    const u8 tf[8] = { in->features, in->count, in->lba_low, in->lba_mid, in->lba_high, in->device, in->command, 0 };
    memcpy(b.apt.CurrentTaskFile, tf, 8);
    if (dir == ATA_DATA_OUT) memcpy(b.data, data, size);
    const DWORD len = (DWORD)(offsetof(Buf, data) + size);
    DWORD got = 0;
    if (!DeviceIoControl(d->h, IOCTL_ATA_PASS_THROUGH, &b, len, &b, len, &got, nullptr)) return false;
    const u8* r = b.apt.CurrentTaskFile;     // error, count, lba low / mid / high, device, status
    if (r[6] & 0x01) return false;           // ERR bit
    if (out) {
        *out = { r[0], r[1], r[2], r[3], r[4], r[5], r[6] };
        *have_out = true;
    }
    if (dir == ATA_DATA_IN) memcpy(data, b.data, size);
    return true;
}

// Legacy SMART IOCTLs: IDENTIFY and SMART subcommands only. `code`: SMART_RCV_DRIVE_DATA(_EX) for data-in,
// SMART_SEND_DRIVE_COMMAND for the rest.
static bool smart_ioctl(HANDLE h, DWORD code, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    if (dir == ATA_DATA_OUT || sectors > 1 || (in->command != ATA_CMD_IDENTIFY && in->command != ATA_CMD_SMART)) return false;
    SENDCMDINPARAMS inp = {};
    inp.cBufferSize = sectors * 512;
    inp.irDriveRegs = { in->features, in->count, in->lba_low, in->lba_mid, in->lba_high, 0xA0, in->command, 0 };
    alignas(8) u8 ob[sizeof(SENDCMDOUTPARAMS) + 512] = {};
    const DWORD out_size = (DWORD)(sizeof(SENDCMDOUTPARAMS) - 1 + (dir == ATA_DATA_IN ? 512 : sizeof(IDEREGS)));
    DWORD got = 0;
    if (!DeviceIoControl(h, code, &inp, sizeof(SENDCMDINPARAMS) - 1, ob, out_size, &got, nullptr)) return false;
    const SENDCMDOUTPARAMS* o = (const SENDCMDOUTPARAMS*)ob;
    if (o->DriverStatus.bDriverError) return false;
    if (dir == ATA_DATA_IN) {
        memcpy(data, o->bBuffer, 512);
    } else if (out && got >= sizeof(SENDCMDOUTPARAMS) - 1 + sizeof(IDEREGS)) {
        const IDEREGS* r = (const IDEREGS*)o->bBuffer;
        *out = { r->bFeaturesReg, r->bSectorCountReg, r->bSectorNumberReg, r->bCylLowReg, r->bCylHighReg, r->bDriveHeadReg, r->bCommandReg };
        *have_out = true;
    }
    return true;
}

static ScsiDir scsi_dir(AtaDir dir) { return dir == ATA_DATA_IN ? SCSI_IN : dir == ATA_DATA_OUT ? SCSI_OUT : SCSI_NONE; }

static bool sat(AtaDev* d, bool twelve, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    u8 cdb[16] = {};
    const u8 protocol = dir == ATA_DATA_IN ? 4 : dir == ATA_DATA_OUT ? 5 : 3;     // PIO in / PIO out / non-data
    const bool ck_cond = out && dir == ATA_NO_DATA;                                 // return the registers
    const u8 flags = (u8)((ck_cond ? 0x20 : 0) | (dir == ATA_DATA_IN ? 0x08 : 0) | (dir != ATA_NO_DATA ? 0x06 : 0));
    if (twelve) {
        const u8 c[10] = { 0xA1, (u8)(protocol << 1), flags, in->features, in->count, in->lba_low, in->lba_mid, in->lba_high, in->device, in->command };
        memcpy(cdb, c, sizeof(c));
    } else {
        cdb[0] = 0x85;
        cdb[1] = (u8)(protocol << 1);
        cdb[2] = flags;
        cdb[4] = in->features;
        cdb[6] = in->count;
        cdb[8] = in->lba_low;
        cdb[10] = in->lba_mid;
        cdb[12] = in->lba_high;
        cdb[13] = in->device;
        cdb[14] = in->command;
    }
    u8 status = 0, sense[SCSI_SENSE_LEN] = {};
    if (!scsi_cmd(d->h, cdb, twelve ? 12 : 16, data, sectors * 512, scsi_dir(dir), &status, sense)) return false;
    if (status == 0) return true;
    if (status != 2 || !ck_cond) return false;
    // CHECK CONDITION with "ATA pass-through information available": the registers are in the sense data.
    const u8 rc = sense[0] & 0x7F;
    if (rc >= 0x72) {
        const u32 end = 8 + sense[7] < SCSI_SENSE_LEN ? 8 + sense[7] : SCSI_SENSE_LEN;
        for (u32 p = 8; p + 14 <= end; p += 2 + sense[p + 1]) {
            if (sense[p] != 0x09) continue;
            const u8* a = sense + p;
            *out = { a[3], a[5], a[7], a[9], a[11], a[12], a[13] };
            *have_out = true;
            return true;
        }
    } else if ((rc == 0x70 || rc == 0x71) && sense[12] == 0 && sense[13] == 0x1D && sense[4]) {
        *out = { sense[3], sense[6], sense[11], sense[10], sense[9], sense[5], sense[4] };
        *have_out = true;
        return true;
    }
    return false;
}

static bool jmicron(AtaDev* d, bool prolific, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    const bool smart_status = in->command == ATA_CMD_SMART && in->features == SUB_RETURN_STATUS;
    u8 status_byte = 0;
    u32 len = sectors * 512;
    if (smart_status) { data = &status_byte; len = 1; dir = ATA_DATA_IN; }     // the bridge returns 0xC2 / 0x2C
    const u8 cdb[14] = { 0xDF, (u8)(dir == ATA_DATA_OUT ? 0x00 : 0x10), 0, (u8)(len >> 8), (u8)len, in->features, in->count,
                         in->lba_low, in->lba_mid, in->lba_high, (u8)(in->device | (d->port ? 0xB0 : 0xA0)), in->command, 0x06, 0x7B };
    u8 status = 0, sense[SCSI_SENSE_LEN];
    if (!scsi_cmd(d->h, cdb, prolific ? 14 : 12, data, len, scsi_dir(dir), &status, sense) || status != 0) return false;
    if (smart_status && out) {
        if (status_byte != 0xC2 && status_byte != 0x2C) return false;
        *out = { 0, 0, 0, (u8)(status_byte == 0xC2 ? 0x4F : 0xF4), status_byte, 0, 0x50 };
        *have_out = true;
    }
    return true;
}

static bool sunplus(AtaDev* d, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    const u32 len = sectors * 512;
    const u8 cdb[12] = { 0xF8, 0x00, 0x22, (u8)(dir == ATA_DATA_IN ? 0x10 : dir == ATA_DATA_OUT ? 0x11 : 0x00), (u8)(len >> 9),
                         in->features, in->count, in->lba_low, in->lba_mid, in->lba_high, (u8)(in->device | 0xA0), in->command };
    u8 status = 0, sense[SCSI_SENSE_LEN];
    if (!scsi_cmd(d->h, cdb, 12, data, len, scsi_dir(dir), &status, sense) || status != 0) return false;
    if (out) {      // "get status" subcommand returns the output registers
        const u8 get[12] = { 0xF8, 0x00, 0x21 };
        u8 regs[8] = {};
        if (scsi_cmd(d->h, get, 12, regs, sizeof(regs), SCSI_IN, &status, sense) && status == 0) {
            *out = { regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7] };
            *have_out = true;
        }
    }
    return true;
}

static bool cypress(AtaDev* d, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    u8 cdb[16] = { 0x24, 0x24, (u8)(in->command == ATA_CMD_IDENTIFY ? 0x80 : 0x00), 0xBE, 1, 0, in->features, in->count,
                   in->lba_low, in->lba_mid, in->lba_high, 0, in->command };
    u8 status = 0, sense[SCSI_SENSE_LEN];
    if (!scsi_cmd(d->h, cdb, 16, data, sectors * 512, scsi_dir(dir), &status, sense) || status != 0) return false;
    if (out) {      // read back the task file
        u8 rb[16] = { 0x24, 0x24, 0x01 }, regs[8] = {};
        if (scsi_cmd(d->h, rb, 16, regs, sizeof(regs), SCSI_IN, &status, sense) && status == 0) {
            *out = { regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7] };
            *have_out = true;
        }
    }
    return true;
}

bool ata_cmd(AtaDev* d, const AtaRegs* in_regs, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    bool dummy = false;
    if (!have_out) have_out = &dummy;
    *have_out = false;
    AtaRegs in = *in_regs;
    if (dir != ATA_NO_DATA && in.count == 0) in.count = (u8)sectors;     // bridges size the transfer from it
    switch (d->tr) {
        case ATA_TR_ATA_PT: return ata_pt(d, &in, out, have_out, data, sectors, dir);
        case ATA_TR_SMART_IOCTL:
            return smart_ioctl(d->h, dir == ATA_DATA_IN ? SMART_RCV_DRIVE_DATA : SMART_SEND_DRIVE_COMMAND, &in, out, have_out, data,
                               sectors, dir);
        case ATA_TR_SAT16: return sat(d, false, &in, out, have_out, data, sectors, dir);
        case ATA_TR_SAT12: return sat(d, true, &in, out, have_out, data, sectors, dir);
        case ATA_TR_JMICRON: return jmicron(d, false, &in, out, have_out, data, sectors, dir);
        case ATA_TR_PROLIFIC: return jmicron(d, true, &in, out, have_out, data, sectors, dir);
        case ATA_TR_SUNPLUS: return sunplus(d, &in, out, have_out, data, sectors, dir);
        case ATA_TR_CYPRESS: return cypress(d, &in, out, have_out, data, sectors, dir);
        case ATA_TR_CSMI: return csmi_ata_cmd(d, &in, out, have_out, data, sectors, dir);
        default: return false;
    }
}

// ---- commands -------------------------------------------------------------------------------------

static bool smart_in(AtaDev* d, u8 feature, u8 lba_low, u8 out[512]) {
    const AtaRegs r = { feature, 1, lba_low, 0x4F, 0xC2, 0, ATA_CMD_SMART };
    memset(out, 0, 512);
    return ata_cmd(d, &r, nullptr, nullptr, out, 1, ATA_DATA_IN);
}

// IDENTIFY data looks real: ATA device (word 0 bit 15 clear), checksum OK when present, a model string.
static bool identify_valid(const u8 id[512]) {
    if (id[1] & 0x80) return false;
    if (id[510] == 0xA5) {
        u8 sum = 0;
        for (u32 i = 0; i < 512; ++i) sum = (u8)(sum + id[i]);
        if (sum) return false;
    }
    for (u32 i = 54; i < 94; ++i)
        if (id[i] > ' ' && id[i] < 0x7F) return true;
    return false;
}

bool ata_identify(AtaDev* d, u8 out[512]) {
    const AtaRegs r = { 0, 1, 0, 0, 0, 0, ATA_CMD_IDENTIFY };
    memset(out, 0, 512);
    return ata_cmd(d, &r, nullptr, nullptr, out, 1, ATA_DATA_IN) && identify_valid(out);
}

bool ata_smart_read_data(AtaDev* d, u8 out[512]) { return smart_in(d, SUB_READ_DATA, 0, out); }
bool ata_smart_read_thresholds(AtaDev* d, u8 out[512]) { return smart_in(d, SUB_READ_THRESHOLDS, 0, out); }
bool ata_smart_read_log(AtaDev* d, u8 address, u8 out[512]) { return smart_in(d, SUB_READ_LOG, address, out); }

i32 ata_smart_status(AtaDev* d) {
    const AtaRegs r = { SUB_RETURN_STATUS, 0, 0, 0x4F, 0xC2, 0, ATA_CMD_SMART };
    AtaRegs o = {};
    bool have = false;
    if (!ata_cmd(d, &r, &o, &have, nullptr, 0, ATA_NO_DATA) || !have) return -1;
    if (o.lba_mid == 0x4F && o.lba_high == 0xC2) return 0;
    if (o.lba_mid == 0xF4 && o.lba_high == 0x2C) return 1;
    return -1;
}

bool ata_smart_execute(AtaDev* d, u8 subcommand) {
    const AtaRegs r = { SUB_EXECUTE_OFFLINE, 0, subcommand, 0x4F, 0xC2, 0, ATA_CMD_SMART };
    return ata_cmd(d, &r, nullptr, nullptr, nullptr, 0, ATA_NO_DATA);
}

bool ata_set_features(AtaDev* d, u8 feature, u8 count) {
    const AtaRegs r = { feature, count, 0, 0, 0, 0, ATA_CMD_SET_FEATURES };
    return ata_cmd(d, &r, nullptr, nullptr, nullptr, 0, ATA_NO_DATA);
}

bool ata_attach(AtaDev* d, HANDLE rw, u32 bus_type, u16 usb_vid, u16 usb_pid, u8 id[512]) {
    AtaTransport order[6];
    u32 n = 0;
    const bool usb = bus_type == BusTypeUsb || usb_vid != 0;
    if (usb) {
        switch (usb_vid) {
            case 0x152D: order[n++] = ATA_TR_SAT16; order[n++] = ATA_TR_SAT12; order[n++] = ATA_TR_JMICRON; break;   // JMicron
            case 0x067B: order[n++] = ATA_TR_SAT16; order[n++] = ATA_TR_PROLIFIC; break;                            // Prolific
            case 0x04FC: order[n++] = ATA_TR_SUNPLUS; order[n++] = ATA_TR_SAT16; break;                             // Sunplus
            case 0x04B4: order[n++] = ATA_TR_CYPRESS; order[n++] = ATA_TR_SAT16; break;                             // Cypress
            default: order[n++] = ATA_TR_SAT16; order[n++] = ATA_TR_SAT12; break;  // ASMedia, Realtek, VIA, Genesys...
        }
    } else if (bus_type == BusTypeScsi || bus_type == BusTypeSas) {
        order[n++] = ATA_TR_SAT16;
        order[n++] = ATA_TR_SAT12;
        order[n++] = ATA_TR_ATA_PT;
    } else {
        order[n++] = ATA_TR_ATA_PT;
        order[n++] = ATA_TR_SMART_IOCTL;
        order[n++] = ATA_TR_SAT16;
    }
    (void)usb_pid;
    for (u32 i = 0; i < n; ++i) {
        const u32 ports = order[i] == ATA_TR_JMICRON || order[i] == ATA_TR_PROLIFIC ? 2 : 1;
        for (u32 port = 0; port < ports; ++port) {
            *d = {};
            d->h = rw;
            d->tr = order[i];
            d->port = (u8)port;
            if (ata_identify(d, id)) return true;
        }
    }
    *d = {};
    return false;
}

// ---- query-only paths ------------------------------------------------------------------------------

static bool ata_protocol_query(HANDLE h, u32 type, u32 value, u32 sub, u8 out[512]) {
    constexpr u32 len = (u32)offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512;
    alignas(8) u8 buf[len] = {};
    STORAGE_PROPERTY_QUERY* q = (STORAGE_PROPERTY_QUERY*)buf;
    STORAGE_PROTOCOL_SPECIFIC_DATA* p = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    p->ProtocolType = ProtocolTypeAta;
    p->DataType = type;
    p->ProtocolDataRequestValue = value;
    p->ProtocolDataRequestSubValue = sub;
    p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    p->ProtocolDataLength = 512;
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, len, buf, len, &got, nullptr)) return false;
    const STORAGE_PROTOCOL_DATA_DESCRIPTOR* d = (const STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buf;
    const STORAGE_PROTOCOL_SPECIFIC_DATA* r = &d->ProtocolSpecificData;
    if (r->ProtocolDataOffset < sizeof(*r) || r->ProtocolDataLength < 512 ||
        offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) + r->ProtocolDataOffset + 512 > len)
        return false;
    memcpy(out, (const u8*)r + r->ProtocolDataOffset, 512);
    return true;
}

bool ata_query_identify(HANDLE h, u8 out[512]) { return ata_protocol_query(h, AtaDataTypeIdentify, 0, 0, out) && identify_valid(out); }
bool ata_query_log(HANDLE h, u8 log, u8 page, u8 out[512]) { return ata_protocol_query(h, AtaDataTypeLogPage, log, page, out); }

bool ata_query_smart(HANDLE h, u8 features, u8 out[512]) {
    const AtaRegs r = { features, 1, 0, 0x4F, 0xC2, 0, ATA_CMD_SMART };
    return smart_ioctl(h, SMART_RCV_DRIVE_DATA_EX, &r, nullptr, nullptr, out, 1, ATA_DATA_IN);
}

// ---- IDENTIFY / SMART parsing ---------------------------------------------------------------------------

static u16 word(const u8* id, u32 w) { return (u16)(id[w * 2] | (id[w * 2 + 1] << 8)); }

// ATA strings are byte-swapped word pairs, space padded.
static void ata_string(const u8* id, u32 first_word, u32 words, char* out) {
    u32 n = 0;
    for (u32 w = 0; w < words; ++w) {
        out[n++] = (char)id[(first_word + w) * 2 + 1];
        out[n++] = (char)id[(first_word + w) * 2];
    }
    out[n] = 0;
    while (n && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
    u32 s = 0;
    while (out[s] == ' ') ++s;
    if (s) memmove(out, out + s, n - s + 1);
}

void ata_parse_identify(const u8* id, AtaIdInfo* o) {
    memset(o, 0, sizeof(*o));
    ata_string(id, 27, 20, o->model);
    ata_string(id, 10, 10, o->serial);
    ata_string(id, 23, 4, o->firmware);
    const u16 w49 = word(id, 49), w76 = word(id, 76), w77 = word(id, 77), w78 = word(id, 78), w80 = word(id, 80);
    const u16 w82 = word(id, 82), w83 = word(id, 83), w84 = word(id, 84), w85 = word(id, 85), w86 = word(id, 86);
    const u16 w106 = word(id, 106), w222 = word(id, 222);
    o->lba48 = (w83 & 0x4400) == 0x4400;
    o->sectors = o->lba48 ? (u64)word(id, 100) | ((u64)word(id, 101) << 16) | ((u64)word(id, 102) << 32) | ((u64)word(id, 103) << 48)
                          : (w49 & 0x200) ? (u64)word(id, 60) | ((u64)word(id, 61) << 16) : 0;
    o->logical_sector = o->physical_sector = 512;
    if ((w106 & 0xC000) == 0x4000) {
        if (w106 & 0x1000) o->logical_sector = (u32)(word(id, 117) | (word(id, 118) << 16)) * 2;
        if (w106 & 0x2000) o->physical_sector = o->logical_sector << (w106 & 0xF);
        else o->physical_sector = o->logical_sector;
    }
    const u16 rpm = word(id, 217);
    o->rotation = rpm == 1 ? 1 : (rpm >= 0x401 && rpm < 0xFFFF) ? rpm : 0;
    o->buffer_bytes = word(id, 21) < 0xFFFF ? word(id, 21) * 512u : 0;
    if (w76 && w76 != 0xFFFF) {
        o->sata_gen_max = (w76 & 0x8) ? 3 : (w76 & 0x4) ? 2 : (w76 & 0x2) ? 1 : 0;
        o->ncq = (w76 & 0x100) != 0;
        o->queue_depth = (u8)((word(id, 75) & 0x1F) + 1);
        if (w77 && w77 != 0xFFFF) o->sata_gen_cur = (u8)((w77 >> 1) & 0x7);
        if (w78 && w78 != 0xFFFF) o->devsleep = (w78 & 0x100) != 0;
    }
    o->smart = (w82 & 0x1) != 0;
    o->security = (w82 & 0x2) != 0;
    o->write_cache = (w82 & 0x20) != 0;
    o->smart_enabled = (w85 & 0x1) != 0;
    o->write_cache_enabled = (w85 & 0x20) != 0;
    o->apm = (w83 & 0x8) != 0;
    o->aam = (w83 & 0x200) != 0;
    o->apm_enabled = (w86 & 0x8) != 0;
    o->aam_enabled = (w86 & 0x200) != 0;
    o->gpl = (w84 & 0x20) != 0;
    o->trim = (word(id, 169) & 0x1) != 0;
    o->sct = (word(id, 206) & 0x1) != 0;
    o->sanitize = (word(id, 59) & 0x1000) != 0;
    o->apm_value = (u8)word(id, 91);
    o->aam_value = (u8)word(id, 94);
    o->aam_recommended = (u8)(word(id, 94) >> 8);

    // Standard: the newest major version bit, then the SATA transport version.
    static const char* const majors[] = { nullptr, nullptr, nullptr, nullptr, "ATA/ATAPI-4", "ATA/ATAPI-5", "ATA/ATAPI-6",
                                          "ATA/ATAPI-7", "ATA8-ACS", "ACS-2", "ACS-3", "ACS-4", "ACS-5" };
    const char* major = nullptr;
    if (w80 && w80 != 0xFFFF)
        for (u32 b = 4; b < ARRAY_COUNT(majors); ++b)
            if (w80 & (1u << b)) major = majors[b];
    static const char* const sata[] = { "ATA8-AST", "SATA 1.0a", "SATA II Ext", "SATA 2.5", "SATA 2.6", "SATA 3.0",
                                        "SATA 3.1", "SATA 3.2", "SATA 3.3", "SATA 3.4", "SATA 3.5" };
    const char* trans = nullptr;
    if (w222 && w222 != 0xFFFF && (w222 >> 12) == 1)
        for (u32 b = 0; b < ARRAY_COUNT(sata); ++b)
            if (w222 & (1u << b)) trans = sata[b];
    snprintf(o->standard, sizeof(o->standard), "%s%s%s", major ? major : "", major && trans ? " | " : "", trans ? trans : "");
}

void ata_parse_smart_caps(const u8* s, AtaSmartCaps* o) {
    memset(o, 0, sizeof(*o));
    o->offline_status = s[362];
    o->selftest_status = s[363];
    const u8 cap = s[367];
    o->can_offline = (cap & 0x01) != 0;
    o->can_selftest = (cap & 0x10) != 0;
    o->can_conveyance = (cap & 0x20) != 0;
    o->can_selective = (cap & 0x40) != 0;
    o->error_log = (s[370] & 0x01) != 0;
    o->poll_short_min = s[372];
    o->poll_extended_min = s[373] == 0xFF ? (u16)(s[375] | (s[376] << 8)) : s[373];
    o->poll_conveyance_min = s[374];
}
