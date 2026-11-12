// Physical disk enumeration and temperature via storage IOCTLs (IOCTL_STORAGE_QUERY_PROPERTY); device-tree
// location via SetupAPI; partitions and volumes.
#include "core/physdisk.h"
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>     // defines the DEVPKEY_* below in this file
#include <devpkey.h>
#include <pciprop.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

// ---- device tree ----------------------------------------------------------------------------------

static const GUID DISK_INTERFACE = { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

static bool dev_prop(HDEVINFO set, SP_DEVINFO_DATA* dev, const DEVPROPKEY* key, void* out, DWORD size) {
    DEVPROPTYPE type;
    return SetupDiGetDevicePropertyW(set, dev, key, &type, (PBYTE)out, size, nullptr, 0) != FALSE;
}

bool physdisk_location(u32 number, DiskLocation* out) {
    memset(out, 0, sizeof(*out));
    HDEVINFO set = SetupDiGetClassDevsW(&DISK_INTERFACE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    wchar_t inst[256] = {};
    alignas(8) u8 detail_buf[1024];
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    for (DWORD i = 0; !inst[0] && SetupDiEnumDeviceInterfaces(set, nullptr, &DISK_INTERFACE, i, &ifd); ++i) {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detail_buf;
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA dev = { sizeof(dev) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(detail_buf), nullptr, &dev)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER dn = {};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &dn, sizeof(dn), &got, nullptr) &&
            dn.DeviceNumber == number)
            dev_prop(set, &dev, &DEVPKEY_Device_InstanceId, inst, sizeof(inst) - sizeof(wchar_t));
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!inst[0]) return false;

    // Walk up the parents: the first "USB\VID_..." is the enclosure bridge, the first "PCI\..." the controller.
    HDEVINFO list = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (list == INVALID_HANDLE_VALUE) return false;
    bool pci_done = false;
    for (u32 depth = 0; depth < 10 && inst[0]; ++depth) {
        SP_DEVINFO_DATA dev = { sizeof(dev) };
        if (!SetupDiOpenDeviceInfoW(list, inst, nullptr, 0, &dev)) break;
        if (!out->usb_vid && !_wcsnicmp(inst, L"USB\\VID_", 8)) {
            out->usb_vid = (u16)wcstoul(inst + 8, nullptr, 16);
            if (const wchar_t* p = wcsstr(inst, L"PID_")) out->usb_pid = (u16)wcstoul(p + 4, nullptr, 16);
        }
        if (!pci_done && !_wcsnicmp(inst, L"PCI\\", 4)) {
            pci_done = true;
            u32 v = 0;
            if (dev_prop(list, &dev, &DEVPKEY_PciDevice_CurrentLinkSpeed, &v, sizeof(v))) out->pcie_gen = (u8)v;
            if (dev_prop(list, &dev, &DEVPKEY_PciDevice_CurrentLinkWidth, &v, sizeof(v))) out->pcie_width = (u8)v;
            if (dev_prop(list, &dev, &DEVPKEY_PciDevice_MaxLinkSpeed, &v, sizeof(v))) out->pcie_gen_max = (u8)v;
            if (dev_prop(list, &dev, &DEVPKEY_PciDevice_MaxLinkWidth, &v, sizeof(v))) out->pcie_width_max = (u8)v;
            wchar_t name[96] = {};
            if (dev_prop(list, &dev, &DEVPKEY_Device_FriendlyName, name, sizeof(name) - sizeof(wchar_t)) ||
                dev_prop(list, &dev, &DEVPKEY_Device_DeviceDesc, name, sizeof(name) - sizeof(wchar_t)))
                WideCharToMultiByte(CP_UTF8, 0, name, -1, out->controller, sizeof(out->controller), nullptr, nullptr);
        }
        wchar_t parent[256] = {};
        if (!dev_prop(list, &dev, &DEVPKEY_Device_Parent, parent, sizeof(parent) - sizeof(wchar_t))) break;
        wcscpy_s(inst, parent);
    }
    SetupDiDestroyDeviceInfoList(list);
    return true;
}

// ---- partitions -----------------------------------------------------------------------------------

static void gpt_type_name(const GUID& g, char* out, u32 size) {
    static const struct { u32 data1; const char* name; } types[] = {
        { 0xC12A7328, "EFI system" },   { 0xE3C9E316, "Microsoft reserved" }, { 0xEBD0A0A2, "Basic data" },
        { 0xDE94BBA4, "Recovery" },     { 0x5808C8AA, "LDM metadata" },       { 0xAF9B60A0, "LDM data" },
        { 0xE75CAF8F, "Storage Spaces" }, { 0x0FC63DAF, "Linux data" },       { 0x0657FD6D, "Linux swap" },
    };
    for (u32 i = 0; i < ARRAY_COUNT(types); ++i)
        if (g.Data1 == types[i].data1) { snprintf(out, size, "%s", types[i].name); return; }
    snprintf(out, size, "%08lX", g.Data1);
}

u32 physdisk_partitions(HANDLE h, u32 number, DiskPartition* out, u32 max, char style[8]) {
    snprintf(style, 8, "?");
    alignas(8) static u8 buf[sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX)];
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf, sizeof(buf), &got, nullptr)) return 0;
    const DRIVE_LAYOUT_INFORMATION_EX* lay = (const DRIVE_LAYOUT_INFORMATION_EX*)buf;
    snprintf(style, 8, "%s", lay->PartitionStyle == PARTITION_STYLE_GPT ? "GPT" : lay->PartitionStyle == PARTITION_STYLE_MBR ? "MBR" : "RAW");
    u32 n = 0;
    for (DWORD i = 0; i < lay->PartitionCount && n < max; ++i) {
        const PARTITION_INFORMATION_EX* p = &lay->PartitionEntry[i];
        if (p->PartitionLength.QuadPart == 0) continue;     // empty MBR slots
        if (lay->PartitionStyle == PARTITION_STYLE_MBR && (p->Mbr.PartitionType == 0x05 || p->Mbr.PartitionType == 0x0F)) continue;
        DiskPartition* d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->number = p->PartitionNumber;
        d->offset = (u64)p->StartingOffset.QuadPart;
        d->size = (u64)p->PartitionLength.QuadPart;
        if (p->PartitionStyle == PARTITION_STYLE_GPT) gpt_type_name(p->Gpt.PartitionType, d->type, sizeof(d->type));
        else snprintf(d->type, sizeof(d->type), "0x%02X", p->Mbr.PartitionType);
    }

    // Volumes: match each single-extent volume on this disk to a partition by offset.
    wchar_t vol[MAX_PATH];
    HANDLE f = FindFirstVolumeW(vol, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return n;
    do {
        const size_t len = wcslen(vol);
        if (len < 2) continue;
        vol[len - 1] = 0;       // CreateFile wants no trailing backslash
        HANDLE v = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        vol[len - 1] = L'\\';
        if (v == INVALID_HANDLE_VALUE) continue;
        alignas(8) u8 ext_buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
        const bool ok = DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, ext_buf, sizeof(ext_buf), &got, nullptr);
        CloseHandle(v);
        const VOLUME_DISK_EXTENTS* ext = (const VOLUME_DISK_EXTENTS*)ext_buf;
        if (!ok || ext->NumberOfDiskExtents < 1 || ext->Extents[0].DiskNumber != number) continue;
        for (u32 k = 0; k < n; ++k) {
            if (out[k].offset != (u64)ext->Extents[0].StartingOffset.QuadPart) continue;
            wchar_t names[64] = {}, label[64] = {}, fs[32] = {};
            DWORD names_len = 0;
            if (GetVolumePathNamesForVolumeNameW(vol, names, ARRAY_COUNT(names), &names_len) && names[0] && names[1] == L':')
                out[k].letter = (char)names[0];
            if (GetVolumeInformationW(vol, label, ARRAY_COUNT(label), nullptr, nullptr, nullptr, fs, ARRAY_COUNT(fs))) {
                WideCharToMultiByte(CP_UTF8, 0, fs, -1, out[k].fs, sizeof(out[k].fs), nullptr, nullptr);
                WideCharToMultiByte(CP_UTF8, 0, label, -1, out[k].label, sizeof(out[k].label), nullptr, nullptr);
            }
        }
    } while (FindNextVolumeW(f, vol, MAX_PATH));
    FindVolumeClose(f);
    return n;
}

// ---- adapter / Windows flags ------------------------------------------------------------------------

static bool query_prop(HANDLE h, STORAGE_PROPERTY_ID id, void* out, u32 size) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = id;
    q.QueryType = PropertyStandardQuery;
    DWORD got = 0;
    memset(out, 0, size);
    return DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), out, size, &got, nullptr) && got >= 8;
}

bool physdisk_adapter(HANDLE h, u16* bus_major, u16* bus_minor, bool* queueing) {
    STORAGE_ADAPTER_DESCRIPTOR a;
    if (!query_prop(h, StorageAdapterProperty, &a, sizeof(a)) || a.Size < offsetof(STORAGE_ADAPTER_DESCRIPTOR, BusMinorVersion) + 2)
        return false;
    *bus_major = a.BusMajorVersion;
    *bus_minor = a.BusMinorVersion;
    *queueing = a.CommandQueueing != 0;
    return true;
}

void physdisk_flags(HANDLE h, i32* trim, i32* write_cache, i32* seek_penalty) {
    *trim = *write_cache = *seek_penalty = -1;
    DEVICE_TRIM_DESCRIPTOR t;
    if (query_prop(h, StorageDeviceTrimProperty, &t, sizeof(t))) *trim = t.TrimEnabled ? 1 : 0;
    STORAGE_WRITE_CACHE_PROPERTY w;
    if (query_prop(h, StorageDeviceWriteCacheProperty, &w, sizeof(w)) && w.WriteCacheEnabled != WriteCacheEnableUnknown)
        *write_cache = w.WriteCacheEnabled == WriteCacheEnabled ? 1 : 0;
    DEVICE_SEEK_PENALTY_DESCRIPTOR s;
    if (query_prop(h, StorageDeviceSeekPenaltyProperty, &s, sizeof(s))) *seek_penalty = s.IncursSeekPenalty ? 1 : 0;
}
