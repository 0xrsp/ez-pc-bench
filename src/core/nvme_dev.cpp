// NVMe admin queries (see nvme_dev.h).
#include "core/nvme_dev.h"
#include "core/scsi_pt.h"
#include <windows.h>
#include <winioctl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

const char* const NVME_TRANSPORT_NAMES[NVME_TR_COUNT] = {
    "none", "Windows NVMe driver", "JMicron JMS583 USB bridge", "ASMedia ASM2362 USB bridge", "Realtek RTL9210 USB bridge",
};

constexpr u8 NVME_OP_GET_LOG = 0x02, NVME_OP_IDENTIFY = 0x06, NVME_OP_SELF_TEST = 0x14;

// ---- Windows (stornvme) ------------------------------------------------------------------------------

constexpr u32 PROTO_MAX = 4096;

static bool win_query(HANDLE h, u32 type, u32 value, u32 sub, u32 len, void* out, u32* dw0) {
    constexpr u32 head = (u32)offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    alignas(8) static u8 buf[head + PROTO_MAX];
    if (len > PROTO_MAX) return false;
    memset(buf, 0, head + len);
    STORAGE_PROPERTY_QUERY* q = (STORAGE_PROPERTY_QUERY*)buf;
    STORAGE_PROTOCOL_SPECIFIC_DATA* p = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
    // Device-level first; some drivers only answer the adapter-level query.
    static const STORAGE_PROPERTY_ID ids[] = { StorageDeviceProtocolSpecificProperty, StorageAdapterProtocolSpecificProperty };
    for (u32 k = 0; k < ARRAY_COUNT(ids); ++k) {
        memset(buf, 0, head + len);
        q->PropertyId = ids[k];
        q->QueryType = PropertyStandardQuery;
        p->ProtocolType = ProtocolTypeNvme;
        p->DataType = type;
        p->ProtocolDataRequestValue = value;
        p->ProtocolDataRequestSubValue = sub;
        p->ProtocolDataOffset = len ? sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) : 0;
        p->ProtocolDataLength = len;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, head + len, buf, head + len, &got, nullptr)) continue;
        const STORAGE_PROTOCOL_DATA_DESCRIPTOR* d = (const STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buf;
        const STORAGE_PROTOCOL_SPECIFIC_DATA* r = &d->ProtocolSpecificData;
        if (d->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) continue;
        if (dw0) *dw0 = r->FixedProtocolReturnData;
        if (len) {
            if (r->ProtocolDataOffset < sizeof(*r) || r->ProtocolDataLength < len ||
                offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) + r->ProtocolDataOffset + len > head + PROTO_MAX)
                continue;
            memcpy(out, (const u8*)r + r->ProtocolDataOffset, len);
        }
        return true;
    }
    return false;
}

static bool win_self_test(HANDLE rw, u8 code, char* err, u32 err_size) {
    constexpr u32 cmd_off = (u32)offsetof(STORAGE_PROTOCOL_COMMAND, Command);
    constexpr u32 err_off = (cmd_off + STORAGE_PROTOCOL_COMMAND_LENGTH_NVME + 7) & ~7u;
    alignas(8) u8 buf[err_off + 64] = {};
    STORAGE_PROTOCOL_COMMAND* pc = (STORAGE_PROTOCOL_COMMAND*)buf;
    pc->Version = STORAGE_PROTOCOL_STRUCTURE_VERSION;
    pc->Length = sizeof(STORAGE_PROTOCOL_COMMAND);
    pc->ProtocolType = ProtocolTypeNvme;
    pc->Flags = STORAGE_PROTOCOL_COMMAND_FLAG_ADAPTER_REQUEST;
    pc->CommandLength = STORAGE_PROTOCOL_COMMAND_LENGTH_NVME;
    pc->ErrorInfoLength = 64;
    pc->ErrorInfoOffset = err_off;
    pc->TimeOutValue = 30;
    pc->CommandSpecific = STORAGE_PROTOCOL_SPECIFIC_NVME_ADMIN_COMMAND;
    u32* c = (u32*)(buf + cmd_off);     // NVMe submission entry: CDW0 = opcode, CDW1 = NSID, CDW10
    c[0] = NVME_OP_SELF_TEST;
    c[1] = 0xFFFFFFFF;                  // controller and all namespaces
    c[10] = code;
    DWORD got = 0;
    if (!DeviceIoControl(rw, IOCTL_STORAGE_PROTOCOL_COMMAND, buf, sizeof(buf), buf, sizeof(buf), &got, nullptr)) {
        snprintf(err, err_size, "IOCTL_STORAGE_PROTOCOL_COMMAND failed (error %lu)", GetLastError());
        return false;
    }
    if (pc->ReturnStatus != STORAGE_PROTOCOL_STATUS_SUCCESS) {
        // Status code 0x1D: a self-test is already running.
        const u32 sc = (pc->ErrorCode >> 1) & 0xFF;
        snprintf(err, err_size, sc == 0x1D ? "a self-test is already in progress" : "the drive rejected the command (status 0x%lx, NVMe 0x%lx)",
                 pc->ReturnStatus, pc->ErrorCode);
        return false;
    }
    return true;
}

// ---- USB bridges -------------------------------------------------------------------------------------

static void be24(u8* p, u32 v) {
    p[0] = (u8)(v >> 16);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)v;
}

// JMicron JMS583: three ATA PASS-THROUGH (12) phases (command block out, data, response in).
static bool jmicron_cmd(HANDLE h, u8 opcode, u32 nsid, u32 cdw10, void* data, u32 size, char* err, u32 err_size) {
    constexpr u32 SIG = 0x454D564E;     // "NVME"
    u32 cmd[128] = {};
    cmd[0] = SIG;
    cmd[2] = opcode;
    cmd[3] = nsid;
    cmd[12] = cdw10;
    u8 cdb[12] = { 0xA1, 0x80 };
    be24(cdb + 3, sizeof(cmd));
    u8 status = 0, sense[SCSI_SENSE_LEN];
    if (!scsi_cmd(h, cdb, 12, cmd, sizeof(cmd), SCSI_OUT, &status, sense) || status) {
        if (err) snprintf(err, err_size, "bridge rejected the command block");
        return false;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA1;
    cdb[1] = (u8)(0x80 | (size ? 0x2 : 0x1));
    be24(cdb + 3, size);
    if (size) memset(data, 0, size);
    if (!scsi_cmd(h, cdb, 12, data, size, size ? SCSI_IN : SCSI_NONE, &status, sense, 60) || status) {
        if (err) snprintf(err, err_size, "bridge data phase failed");
        return false;
    }
    u32 reply[128] = {};
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA1;
    cdb[1] = 0x8F;
    be24(cdb + 3, sizeof(reply));
    if (!scsi_cmd(h, cdb, 12, reply, sizeof(reply), SCSI_IN, &status, sense) || status || reply[0] != SIG) {
        if (err) snprintf(err, err_size, "no response from the bridge");
        return false;
    }
    const u32 nvme_status = reply[5] >> 17;
    if (nvme_status) {
        if (err) snprintf(err, err_size, (nvme_status & 0xFF) == 0x1D ? "a self-test is already in progress" : "NVMe status 0x%x", nvme_status);
        return false;
    }
    return true;
}

static bool asmedia_cmd(HANDLE h, u8 opcode, u32 cdw10, void* data, u32 size) {
    u8 cdb[16] = { 0xE6, opcode, 0, (u8)cdw10, 0, 0, (u8)(cdw10 >> 24), (u8)(cdw10 >> 16) };
    u8 status = 0, sense[SCSI_SENSE_LEN];
    memset(data, 0, size);
    return scsi_cmd(h, cdb, 16, data, size, SCSI_IN, &status, sense) && status == 0;
}

static bool realtek_cmd(HANDLE h, u8 opcode, u32 cdw10, void* data, u32 size) {
    u8 cdb[16] = { 0xE4, (u8)size, (u8)(size >> 8), opcode, (u8)cdw10 };
    u8 status = 0, sense[SCSI_SENSE_LEN];
    memset(data, 0, size);
    return scsi_cmd(h, cdb, 16, data, size, SCSI_IN, &status, sense) && status == 0;
}

static bool bridge_cmd(NvmeDev* d, u8 opcode, u32 nsid, u32 cdw10, void* data, u32 size) {
    switch (d->tr) {
        case NVME_TR_JMICRON: return jmicron_cmd(d->rw, opcode, nsid, cdw10, data, size, nullptr, 0);
        case NVME_TR_ASMEDIA: return asmedia_cmd(d->rw, opcode, cdw10, data, size);
        case NVME_TR_REALTEK: return realtek_cmd(d->rw, opcode, cdw10, data, size);
        default: return false;
    }
}

// ---- API ---------------------------------------------------------------------------------------------

bool nvme_identify(NvmeDev* d, u32 cns, u32 nsid, u8 out[4096]) {
    if (d->tr == NVME_TR_WINDOWS) return win_query(d->h, NVMeDataTypeIdentify, cns, nsid, 4096, out, nullptr);
    if ((d->tr == NVME_TR_ASMEDIA || d->tr == NVME_TR_REALTEK) && cns == 0 && nsid != 1) return false;
    return bridge_cmd(d, NVME_OP_IDENTIFY, nsid, cns, out, 4096);
}

bool nvme_get_log(NvmeDev* d, u8 lid, void* out, u32 size) {
    if (d->tr == NVME_TR_WINDOWS) return win_query(d->h, NVMeDataTypeLogPage, lid, 0, size, out, nullptr);
    u32 n = size;
    if (d->tr == NVME_TR_REALTEK && n > 512) n = 512;           // larger reads return stale data
    if (d->tr == NVME_TR_JMICRON && lid == 6 && n > 0x218) n = 0x218;
    memset(out, 0, size);
    return bridge_cmd(d, NVME_OP_GET_LOG, 0xFFFFFFFF, lid | ((n / 4 - 1) << 16), out, n);
}

bool nvme_get_feature(NvmeDev* d, u8 fid, u32* dw0, void* data, u32 size) {
    if (d->tr != NVME_TR_WINDOWS) return false;
    return win_query(d->h, NVMeDataTypeFeature, fid, 0, data ? size : 0, data, dw0);
}

bool nvme_self_test(NvmeDev* d, u8 code, char* err, u32 err_size) {
    err[0] = 0;
    if (d->tr == NVME_TR_JMICRON) return jmicron_cmd(d->rw, NVME_OP_SELF_TEST, 0xFFFFFFFF, code, nullptr, 0, err, err_size);
    if (d->tr != NVME_TR_WINDOWS) {
        snprintf(err, err_size, "this USB bridge can't pass self-test commands");
        return false;
    }
    if (!d->rw || d->rw == INVALID_HANDLE_VALUE) {
        snprintf(err, err_size, "needs administrator rights");
        return false;
    }
    return win_self_test(d->rw, code, err, err_size);
}

NvmeTransport nvme_bridge_kind(u16 vid, u16 pid) {
    if (vid == 0x152D && (pid == 0x0583 || pid == 0x0562 || pid == 0x0586)) return NVME_TR_JMICRON;
    if (vid == 0x174C && (pid == 0x2362 || pid == 0x2364)) return NVME_TR_ASMEDIA;
    if (vid == 0x0BDA && (pid == 0x9210 || pid == 0x9211 || pid == 0x9220)) return NVME_TR_REALTEK;
    return NVME_TR_NONE;
}

bool nvme_attach_bridge(NvmeDev* d, HANDLE rw, NvmeTransport kind, u8 id[4096]) {
    d->rw = rw;
    d->tr = kind;
    // Identify Controller: a real one has a PCI vendor id and at least one namespace.
    if (nvme_identify(d, 1, 0, id) && (id[0] | id[1]) && (id[516] | id[517] | id[518] | id[519])) return true;
    d->tr = NVME_TR_NONE;
    return false;
}
