// CSMI STP pass-through (see csmi.h). Structures as in the CSMI SAS specification (csmisas.h, pack 8).
#include "core/csmi.h"
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stdio.h>
#include <string.h>

#pragma pack(push, 8)
struct CsmiHeader {             // = SRB_IO_CONTROL
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
};
struct CsmiIdentify {
    u8 device_type, restricted, initiator_protocol, target_protocol;
    u8 restricted2[8];
    u8 sas_address[8];
    u8 phy_id, signal_class;
    u8 reserved[6];
};
struct CsmiPhyEntity {
    CsmiIdentify identify;
    u8 port_id, negotiated_rate, min_rate, max_rate, change_count, auto_discover, features, reserved;
    CsmiIdentify attached;
};
struct CsmiPhyInfoBuffer {
    CsmiHeader header;
    u8 phy_count;
    u8 reserved[3];
    CsmiPhyEntity phy[32];
};
struct CsmiStpBuffer {
    CsmiHeader header;
    u8 phy_id, port_id, connection_rate, reserved;
    u8 dest_sas_address[8];
    u8 reserved2[4];
    u8 command_fis[20];
    u32 flags, data_length;
    u8 connection_status;
    u8 reserved3[3];
    u8 status_fis[20];
    u32 scr[16];
    u32 data_bytes;
    u8 data[512];
};
#pragma pack(pop)

constexpr ULONG CC_CSMI_SAS_GET_PHY_INFO = 20, CC_CSMI_SAS_STP_PASSTHRU = 25;
constexpr u8 CSMI_PROTOCOL_SATA = 0x01, CSMI_PROTOCOL_STP = 0x04;
constexpr u32 CSMI_STP_READ = 0x01, CSMI_STP_WRITE = 0x02, CSMI_STP_UNSPECIFIED = 0x04, CSMI_STP_PIO = 0x10;

static bool csmi_ioctl(HANDLE h, ULONG code, CsmiHeader* hdr, u32 size) {
    hdr->HeaderLength = sizeof(CsmiHeader);
    memcpy(hdr->Signature, "CSMISAS", 8);
    hdr->Timeout = 60;
    hdr->ControlCode = code;
    hdr->ReturnCode = 0;
    hdr->Length = size - sizeof(CsmiHeader);
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_SCSI_MINIPORT, hdr, size, hdr, size, &got, nullptr) && hdr->ReturnCode == 0;
}

bool csmi_ata_cmd(AtaDev* d, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir) {
    if (sectors > 1) return false;
    CsmiStpBuffer* b = (CsmiStpBuffer*)VirtualAlloc(nullptr, sizeof(CsmiStpBuffer), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!b) return false;
    b->phy_id = d->port;
    b->port_id = d->csmi_port;
    memcpy(b->dest_sas_address, d->sas_address, 8);
    // Host-to-device register FIS.
    u8* f = b->command_fis;
    f[0] = 0x27;
    f[1] = 0x80;
    f[2] = in->command;
    f[3] = in->features;
    f[4] = in->lba_low;
    f[5] = in->lba_mid;
    f[6] = in->lba_high;
    f[7] = in->device | 0xA0;
    f[12] = in->count;
    b->flags = dir == ATA_DATA_IN ? CSMI_STP_READ | CSMI_STP_PIO : dir == ATA_DATA_OUT ? CSMI_STP_WRITE | CSMI_STP_PIO : CSMI_STP_UNSPECIFIED;
    b->data_length = sectors * 512;
    if (dir == ATA_DATA_OUT) memcpy(b->data, data, sectors * 512);
    const bool ok = csmi_ioctl(d->h, CC_CSMI_SAS_STP_PASSTHRU, &b->header, sizeof(CsmiStpBuffer));
    if (ok) {
        const u8* s = b->status_fis;    // device-to-host register FIS
        if (out) {
            *out = { s[3], s[12], s[4], s[5], s[6], s[7], s[2] };
            *have_out = true;
        }
        if (dir == ATA_DATA_IN) memcpy(data, b->data, sectors * 512);
    }
    VirtualFree(b, 0, MEM_RELEASE);
    return ok;
}

u32 csmi_scan(CsmiDisk* out, u32 max) {
    u32 n = 0;
    static CsmiPhyInfoBuffer info;
    for (u32 port = 0; port < 16 && n < max; ++port) {
        wchar_t path[32];
        swprintf(path, 32, L"\\\\.\\Scsi%u:", port);
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        memset(&info, 0, sizeof(info));
        bool used = false;
        if (csmi_ioctl(h, CC_CSMI_SAS_GET_PHY_INFO, &info.header, sizeof(info))) {
            for (u32 i = 0; i < info.phy_count && i < 32 && n < max; ++i) {
                const CsmiPhyEntity* p = &info.phy[i];
                if (!(p->attached.target_protocol & (CSMI_PROTOCOL_SATA | CSMI_PROTOCOL_STP))) continue;
                CsmiDisk* d = &out[n];
                memset(d, 0, sizeof(*d));
                d->dev.h = h;
                d->dev.tr = ATA_TR_CSMI;
                d->dev.port = p->attached.phy_id;
                d->dev.csmi_port = p->port_id;
                memcpy(d->dev.sas_address, p->attached.sas_address, 8);
                d->scsi_port = port;
                if (!ata_identify(&d->dev, d->id)) {
                    d->dev.port = p->identify.phy_id;      // some drivers want the controller-side phy
                    if (!ata_identify(&d->dev, d->id)) continue;
                }
                ++n;
                used = true;
            }
        }
        if (!used) CloseHandle(h);
    }
    return n;
}

void csmi_close(CsmiDisk* disks, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        bool shared = false;     // several disks share one controller handle
        for (u32 k = i + 1; k < count; ++k) shared |= disks[k].dev.h == disks[i].dev.h;
        if (!shared && disks[i].dev.h) CloseHandle(disks[i].dev.h);
        disks[i].dev.h = nullptr;
    }
}
