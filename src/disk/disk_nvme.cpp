// NVMe Identify Controller power states (for the wake-up latency test): the drive's advertised entry / exit
// latencies of its power states, read with IOCTL_STORAGE_QUERY_PROPERTY like the health log in disk_health.cpp.
#include "disk/disk_nvme.h"
#include "core/physdisk.h"
#include <windows.h>
#include <winioctl.h>
#include <nvme.h>
#include <stddef.h>
#include <string.h>

static bool identify_controller(HANDLE h, NVME_IDENTIFY_CONTROLLER_DATA* data) {
    constexpr u32 len = (u32)offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
                        NVME_MAX_LOG_SIZE;
    alignas(8) static u8 buf[len];
    static const STORAGE_PROPERTY_ID ids[] = { StorageAdapterProtocolSpecificProperty, StorageDeviceProtocolSpecificProperty };
    for (u32 k = 0; k < ARRAY_COUNT(ids); ++k) {
        memset(buf, 0, sizeof(buf));
        STORAGE_PROPERTY_QUERY* q = (STORAGE_PROPERTY_QUERY*)buf;
        STORAGE_PROTOCOL_SPECIFIC_DATA* p = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
        q->PropertyId = ids[k];
        q->QueryType = PropertyStandardQuery;
        p->ProtocolType = ProtocolTypeNvme;
        p->DataType = NVMeDataTypeIdentify;
        p->ProtocolDataRequestValue = NVME_IDENTIFY_CNS_CONTROLLER;
        p->ProtocolDataRequestSubValue = 0;
        p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
        p->ProtocolDataLength = NVME_MAX_LOG_SIZE;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, len, buf, len, &got, nullptr)) continue;
        const STORAGE_PROTOCOL_DATA_DESCRIPTOR* d = (const STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buf;
        const STORAGE_PROTOCOL_SPECIFIC_DATA* r = &d->ProtocolSpecificData;
        if (d->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) || r->ProtocolDataOffset < sizeof(*r) ||
            r->ProtocolDataLength < sizeof(NVME_IDENTIFY_CONTROLLER_DATA))
            continue;
        memcpy(data, (const u8*)r + r->ProtocolDataOffset, sizeof(*data));
        return true;
    }
    return false;
}

u32 nvme_power_states(i32 phys_disk, NvmePowerState* out, u32 max, bool* apst_supported) {
    *apst_supported = false;
    if (phys_disk < 0) return 0;
    HANDLE h = physdisk_open((u32)phys_disk, false);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static NVME_IDENTIFY_CONTROLLER_DATA id;
    const bool ok = identify_controller(h, &id);
    CloseHandle(h);
    if (!ok) return 0;
    *apst_supported = id.APSTA.Supported != 0;
    u32 n = 0;
    for (u32 i = 0; i <= id.NPSS && i < 32 && n < max; ++i) {
        const NVME_POWER_STATE_DESC* d = &id.PDS[i];
        NvmePowerState* s = &out[n++];
        s->index = i;
        s->non_operational = d->NOPS != 0;
        s->max_power_w = d->MP * (d->MPS ? 0.0001f : 0.01f);
        s->entry_lat_us = d->ENLAT;
        s->exit_lat_us = d->EXLAT;
    }
    return n;
}
