// Drive health (see disk_health.h).
#include "core/disk_health.h"
#include <windows.h>
#include <winioctl.h>
#include <nvme.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// NVMe 128-bit counters, little endian; the high half is zero for any real drive.
static u64 u128_lo(const UCHAR v[16]) {
    u64 x;
    memcpy(&x, v, sizeof(x));
    return x;
}

static bool nvme_health_log(HANDLE h, NVME_HEALTH_INFO_LOG* log) {
    constexpr u32 len = (u32)offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
                        NVME_MAX_LOG_SIZE;
    alignas(8) static u8 buf[len];
    // Device-level first; some drivers only answer the adapter-level query.
    static const STORAGE_PROPERTY_ID ids[] = { StorageDeviceProtocolSpecificProperty, StorageAdapterProtocolSpecificProperty };
    for (u32 k = 0; k < ARRAY_COUNT(ids); ++k) {
        memset(buf, 0, sizeof(buf));
        STORAGE_PROPERTY_QUERY* q = (STORAGE_PROPERTY_QUERY*)buf;
        STORAGE_PROTOCOL_SPECIFIC_DATA* p = (STORAGE_PROTOCOL_SPECIFIC_DATA*)q->AdditionalParameters;
        q->PropertyId = ids[k];
        q->QueryType = PropertyStandardQuery;
        p->ProtocolType = ProtocolTypeNvme;
        p->DataType = NVMeDataTypeLogPage;
        p->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;
        p->ProtocolDataRequestSubValue = 0;
        p->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
        p->ProtocolDataLength = sizeof(NVME_HEALTH_INFO_LOG);
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, len, buf, len, &got, nullptr)) continue;
        const STORAGE_PROTOCOL_DATA_DESCRIPTOR* d = (const STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buf;
        const STORAGE_PROTOCOL_SPECIFIC_DATA* r = &d->ProtocolSpecificData;
        if (d->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) || r->ProtocolDataOffset < sizeof(*r) ||
            r->ProtocolDataLength < sizeof(NVME_HEALTH_INFO_LOG))
            continue;
        memcpy(log, (const u8*)r + r->ProtocolDataOffset, sizeof(*log));
        return true;
    }
    return false;
}

static void add_reason(DiskHealth* h, HealthStatus level, const char* text) {
    if (level > h->status) h->status = level;
    const size_t len = strlen(h->status_text);
    snprintf(h->status_text + len, sizeof(h->status_text) - len, "%s%s", len ? "; " : "", text);
}

static void read_disk(DiskHealth* h) {
    h->status = HEALTH_UNKNOWN;
    if (h->disk.virtual_disk) {
        snprintf(h->status_text, sizeof(h->status_text), "Virtual disk: health is that of the drive holding its file.");
        return;
    }
    HANDLE f = physdisk_open(h->disk.number, true);
    const bool full_access = f != INVALID_HANDLE_VALUE;
    if (!full_access) f = physdisk_open(h->disk.number, false);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(h->status_text, sizeof(h->status_text), "Cannot open the drive (error %lu).", GetLastError());
        return;
    }
    h->have_temp = physdisk_temperature(f, &h->temp_c, &h->warn_c);

    NVME_HEALTH_INFO_LOG log = {};
    if (h->disk.bus_type == BusTypeNvme && nvme_health_log(f, &log)) {
        h->have_nvme = true;
        h->critical_warning = log.CriticalWarning.AsUchar;
        h->spare_pct = log.AvailableSpare;
        h->spare_threshold_pct = log.AvailableSpareThreshold;
        h->used_pct = log.PercentageUsed;
        h->data_read_bytes = u128_lo(log.DataUnitRead) * 512000ull;      // units of 1000 x 512 bytes
        h->data_written_bytes = u128_lo(log.DataUnitWritten) * 512000ull;
        h->power_on_hours = u128_lo(log.PowerOnHours);
        h->power_cycles = u128_lo(log.PowerCycle);
        h->unsafe_shutdowns = u128_lo(log.UnsafeShutdowns);
        h->media_errors = u128_lo(log.MediaErrors);
        h->error_log_entries = u128_lo(log.ErrorInfoLogEntryCount);
        h->warn_temp_minutes = log.WarningCompositeTemperatureTime;
        h->crit_temp_minutes = log.CriticalCompositeTemperatureTime;
        const u16 kelvin = (u16)(log.Temperature[0] | (log.Temperature[1] << 8));
        if (!h->have_temp && kelvin > 200 && kelvin < 400) { h->temp_c = kelvin - 273.15f; h->have_temp = true; }

        h->status = HEALTH_GOOD;
        const auto& cw = log.CriticalWarning;
        if (cw.ReadOnly) add_reason(h, HEALTH_CRITICAL, "the drive has switched to read-only mode");
        if (cw.ReliabilityDegraded) add_reason(h, HEALTH_CRITICAL, "the drive reports degraded reliability");
        if (cw.AvailableSpaceLow) add_reason(h, HEALTH_CRITICAL, "spare blocks are below the drive's threshold");
        if (cw.VolatileMemoryBackupDeviceFailed) add_reason(h, HEALTH_WARNING, "the cache backup (power-loss protection) failed");
        if (cw.TemperatureThreshold) add_reason(h, HEALTH_WARNING, "the drive is outside its temperature limits");
        if (h->media_errors) add_reason(h, HEALTH_WARNING, "unrecovered media errors were logged");
        if (h->used_pct >= 100) add_reason(h, HEALTH_WARNING, "rated endurance used up");
        else if (h->used_pct >= 90) add_reason(h, HEALTH_WARNING, "over 90% of rated endurance used");
        if (h->status == HEALTH_GOOD)
            snprintf(h->status_text, sizeof(h->status_text), "No warnings reported by the drive.");
    } else {
        STORAGE_PREDICT_FAILURE pf = {};
        DWORD got = 0;
        if (DeviceIoControl(f, IOCTL_STORAGE_PREDICT_FAILURE, nullptr, 0, &pf, sizeof(pf), &got, nullptr)) {
            h->have_predict = true;
            h->predict_failure = pf.PredictFailure != 0;
            h->status = h->predict_failure ? HEALTH_CRITICAL : HEALTH_GOOD;
            snprintf(h->status_text, sizeof(h->status_text), h->predict_failure
                         ? "SMART predicts that this drive will fail. Back up its data."
                         : "SMART reports no predicted failure (detailed attributes aren't read for this drive type).");
        } else if (!full_access) {
            snprintf(h->status_text, sizeof(h->status_text), "Run EzPcBench as administrator to read this drive's health data.");
        } else if (h->disk.bus_type == BusTypeNvme) {
            snprintf(h->status_text, sizeof(h->status_text), "The NVMe driver didn't return the health log (error %lu).", GetLastError());
        } else {
            snprintf(h->status_text, sizeof(h->status_text), "This drive or its driver doesn't report health data to Windows.");
        }
    }
    CloseHandle(f);
}

u32 disk_health_scan(DiskHealth* out, u32 max) {
    PhysDisk disks[PHYSDISK_MAX];
    const u32 n = physdisk_enum(disks, PHYSDISK_MAX < max ? PHYSDISK_MAX : max);
    for (u32 i = 0; i < n; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].disk = disks[i];
        read_disk(&out[i]);
    }
    return n;
}
