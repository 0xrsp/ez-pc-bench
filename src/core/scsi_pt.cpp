// SCSI pass-through (see scsi_pt.h).
#include "core/scsi_pt.h"
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stddef.h>
#include <string.h>

struct SptBuffer {
    SCSI_PASS_THROUGH spt;
    ULONG filler;                   // keeps sense / data aligned
    u8 sense[SCSI_SENSE_LEN];
    u8 data[SCSI_MAX_DATA];
};

bool scsi_cmd(HANDLE h, const u8* cdb, u32 cdb_len, void* data, u32 size, ScsiDir dir, u8* status, u8 sense[SCSI_SENSE_LEN],
              u32 timeout_s) {
    if (cdb_len > 16 || size > SCSI_MAX_DATA) return false;
    SptBuffer* b = (SptBuffer*)VirtualAlloc(nullptr, sizeof(SptBuffer), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!b) return false;
    b->spt.Length = sizeof(SCSI_PASS_THROUGH);
    b->spt.CdbLength = (UCHAR)cdb_len;
    b->spt.SenseInfoLength = SCSI_SENSE_LEN;
    b->spt.SenseInfoOffset = offsetof(SptBuffer, sense);
    b->spt.DataIn = dir == SCSI_IN ? SCSI_IOCTL_DATA_IN : dir == SCSI_OUT ? SCSI_IOCTL_DATA_OUT : SCSI_IOCTL_DATA_UNSPECIFIED;
    b->spt.DataTransferLength = size;
    b->spt.DataBufferOffset = offsetof(SptBuffer, data);
    b->spt.TimeOutValue = timeout_s;
    memcpy(b->spt.Cdb, cdb, cdb_len);
    if (dir == SCSI_OUT && size) memcpy(b->data, data, size);
    const DWORD len = (DWORD)(offsetof(SptBuffer, data) + size);
    DWORD got = 0;
    const bool ok = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH, b, len, b, len, &got, nullptr) != FALSE;
    if (ok) {
        if (status) *status = b->spt.ScsiStatus;
        if (sense) memcpy(sense, b->sense, SCSI_SENSE_LEN);
        if (dir == SCSI_IN && size) memcpy(data, b->data, size);
    }
    VirtualFree(b, 0, MEM_RELEASE);
    return ok;
}
