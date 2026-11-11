#pragma once
// RAID controllers (Intel RST, AMD RAIDXpert, LSI) that hide their member disks behind a RAID volume: SATA
// members are reached with CSMI (Common Storage Management Interface) STP pass-through on \\.\ScsiN:.
// Best effort and untested (no RAID here); needs administrator rights.
#include "core/ata.h"

constexpr u32 CSMI_MAX = 8;

struct CsmiDisk {
    AtaDev dev;             // tr = ATA_TR_CSMI; dev.h stays open until csmi_close
    u32 scsi_port;          // N in \\.\ScsiN:
    u8 id[512];             // IDENTIFY DEVICE
};

// Finds SATA disks attached to CSMI-capable controllers. Handles stay open in the returned entries.
u32 csmi_scan(CsmiDisk* out, u32 max);
void csmi_close(CsmiDisk* disks, u32 count);
bool csmi_ata_cmd(AtaDev* d, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir);
