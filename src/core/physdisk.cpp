// Physical disk enumeration and temperature via storage IOCTLs (IOCTL_STORAGE_QUERY_PROPERTY).
#include "core/physdisk.h"
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

HANDLE physdisk_open(u32 number, bool read) {
    wchar_t path[64];
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%u", number);
    return CreateFileW(path, read ? GENERIC_READ | GENERIC_WRITE : 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

static const char* bus_name(u32 t) {
    switch (t) {
        case BusTypeScsi: return "SCSI";
        case BusTypeAtapi: return "ATAPI";
        case BusTypeAta: return "ATA";
        case BusType1394: return "1394";
        case BusTypeSsa: return "SSA";
        case BusTypeFibre: return "Fibre";
        case BusTypeUsb: return "USB";
        case BusTypeRAID: return "RAID";
        case BusTypeiScsi: return "iSCSI";
        case BusTypeSas: return "SAS";
        case BusTypeSata: return "SATA";
        case BusTypeSd: return "SD";
        case BusTypeMmc: return "MMC";
        case BusTypeVirtual: return "Virtual";
        case BusTypeFileBackedVirtual: return "File-backed virtual";
        case BusTypeSpaces: return "Storage Spaces";
        case BusTypeNvme: return "NVMe";
        case BusTypeSCM: return "SCM";
        case BusTypeUfs: return "UFS";
        default: return "Unknown";
    }
}

// Trimmed copy of a string at `offset` in the descriptor (0 = none).
static void desc_str(const u8* base, DWORD size, DWORD offset, char* out, u32 out_size) {
    out[0] = 0;
    if (!offset || offset >= size) return;
    const char* s = (const char*)base + offset;
    while (*s == ' ') ++s;
    snprintf(out, out_size, "%s", s);
    for (size_t n = strlen(out); n && out[n - 1] == ' '; --n) out[n - 1] = 0;
}

static bool query_identity(HANDLE h, PhysDisk* d) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    alignas(8) u8 buf[1024] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, nullptr)) return false;
    const STORAGE_DEVICE_DESCRIPTOR* sd = (const STORAGE_DEVICE_DESCRIPTOR*)buf;
    char vendor[48], product[64];
    desc_str(buf, got, sd->VendorIdOffset, vendor, sizeof(vendor));
    desc_str(buf, got, sd->ProductIdOffset, product, sizeof(product));
    desc_str(buf, got, sd->SerialNumberOffset, d->serial, sizeof(d->serial));
    desc_str(buf, got, sd->ProductRevisionOffset, d->firmware, sizeof(d->firmware));
    // NVMe drives often report "NVMe" or nothing as the vendor; skip it when it adds nothing.
    if (vendor[0] && _stricmp(vendor, "NVMe") != 0 && !strstr(product, vendor)) snprintf(d->model, sizeof(d->model), "%s %s", vendor, product);
    else snprintf(d->model, sizeof(d->model), "%s", product);
    d->bus_type = (u32)sd->BusType;
    snprintf(d->bus, sizeof(d->bus), "%s", bus_name(d->bus_type));
    d->virtual_disk = d->bus_type == BusTypeVirtual || d->bus_type == BusTypeFileBackedVirtual;
    return true;
}

u32 physdisk_enum(PhysDisk* out, u32 max) {
    // Volume letter -> physical disk number.
    i32 letter_disk[26];
    for (u32 i = 0; i < 26; ++i) {
        letter_disk[i] = -1;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t vol[8] = { L'\\', L'\\', L'.', L'\\', (wchar_t)(L'A' + i), L':', 0 };
        HANDLE v = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (v == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER dn = {};
        DWORD got = 0;
        if (DeviceIoControl(v, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &dn, sizeof(dn), &got, nullptr) &&
            dn.DeviceType == FILE_DEVICE_DISK)
            letter_disk[i] = (i32)dn.DeviceNumber;
        CloseHandle(v);
    }

    u32 n = 0;
    for (u32 num = 0; num < 32 && n < max; ++num) {
        HANDLE h = physdisk_open(num, false);
        if (h == INVALID_HANDLE_VALUE) continue;
        PhysDisk* d = &out[n];
        memset(d, 0, sizeof(*d));
        d->number = num;
        if (query_identity(h, d)) {
            DISK_GEOMETRY_EX geo = {};
            DWORD got = 0;
            if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &got, nullptr))
                d->size_bytes = (u64)geo.DiskSize.QuadPart;
            for (u32 i = 0; i < 26; ++i) {
                if (letter_disk[i] != (i32)num) continue;
                const size_t len = strlen(d->letters);
                snprintf(d->letters + len, sizeof(d->letters) - len, "%s%c:", len ? " " : "", 'A' + i);
            }
            ++n;
        }
        CloseHandle(h);
    }
    return n;
}

bool physdisk_temperature(HANDLE h, f32* temp_c, f32* warn_c) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceTemperatureProperty;
    q.QueryType = PropertyStandardQuery;
    alignas(8) u8 buf[512] = {};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, nullptr)) return false;
    const STORAGE_TEMPERATURE_DATA_DESCRIPTOR* t = (const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf;
    if (got < sizeof(*t) || t->InfoCount == 0) return false;
    const SHORT c = t->TemperatureInfo[0].Temperature;      // the first entry is the overall / composite one
    if (c <= -100 || c >= 200) return false;
    *temp_c = (f32)c;
    *warn_c = t->WarningTemperature > 0 && t->WarningTemperature < 200 ? (f32)t->WarningTemperature : 0.0f;
    return true;
}
