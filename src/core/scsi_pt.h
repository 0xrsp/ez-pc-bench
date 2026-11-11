#pragma once
// SCSI pass-through (IOCTL_SCSI_PASS_THROUGH) for USB enclosures: SAT ATA PASS-THROUGH and the bridge vendors'
// own CDBs (core/ata.cpp, core/nvme_dev.cpp). Needs a read/write handle (administrator).
#include "core/types.h"

typedef void* HANDLE;

enum ScsiDir : u8 { SCSI_NONE, SCSI_IN, SCSI_OUT };
constexpr u32 SCSI_MAX_DATA = 8192;
constexpr u32 SCSI_SENSE_LEN = 32;

// Sends one CDB (<= 16 bytes) with up to SCSI_MAX_DATA bytes of data. True when the IOCTL went through; the
// SCSI status (0 = GOOD, 2 = CHECK CONDITION) and sense data come back for the caller to judge.
bool scsi_cmd(HANDLE h, const u8* cdb, u32 cdb_len, void* data, u32 size, ScsiDir dir, u8* status, u8 sense[SCSI_SENSE_LEN],
              u32 timeout_s = 15);
