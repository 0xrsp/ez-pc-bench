// Energy meter reader (see emi.h).
#include "core/emi.h"
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <emi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GUID_DEVICE_ENERGY_METER; local copy so this file needs no initguid.h.
static const GUID EMI_IFACE = { 0x45bd8344, 0x7ed6, 0x49cf, { 0xa4, 0x40, 0xc2, 0x76, 0xc9, 0x33, 0xb0, 0x53 } };

static void wide_name(const wchar_t* w, u32 chars, char* out, u32 size) {
    wchar_t tmp[EMI_NAME_MAX * 4 + 1] = {};
    const u32 n = chars < ARRAY_COUNT(tmp) - 1 ? chars : (u32)ARRAY_COUNT(tmp) - 1;
    memcpy(tmp, w, n * sizeof(wchar_t));
    WideCharToMultiByte(CP_UTF8, 0, tmp, -1, out, (int)size, nullptr, nullptr);
}

// Readable names for the RAPL-style channel names Intel's and AMD's meters use ("RAPL_Package0_PKG",
// "RAPL_Package0_Core3_CORE", "..._DRAM", "..._PP1" = graphics, "..._PSYS" = platform). `order` sorts the
// table: package, domains, then cores by number, then anything else.
struct RailName { const char* tag; const char* name; const char* id; u32 order; bool cpu; };
static const RailName RAILS[] = {
    { "_PKG",  "CPU package",      "cpu_pkg",  0, true },
    { "_PP0",  "CPU cores",        "cpu_pp0",  1, true },
    { "_PP1",  "Integrated graphics", "gfx",   2, false },
    { "_GFX",  "Integrated graphics", "gfx",   2, false },
    { "_DRAM", "Memory (DRAM)",    "dram",     3, false },
    { "_PSYS", "Platform (PSYS)",  "psys",     4, false },
};

static void add_channel(EmiReader* r, u8 dev, u16 idx, const char* model, const char* ch) {
    if (r->channel_count >= EMI_MAX_CHANNELS) return;
    EmiChannel* c = &r->ch[r->channel_count++];
    memset(c, 0, sizeof(*c));
    c->device = dev;
    c->index = idx;
    c->watts = NAN;
    snprintf(c->raw, sizeof(c->raw), "%s%s%s", model, model[0] ? " " : "", ch);
    c->order = 900 + r->channel_count;
    const char* core = strstr(ch, "_Core");
    if (core && strstr(ch, "_CORE") && core[5] >= '0' && core[5] <= '9') {
        const u32 n = (u32)atoi(core + 5);
        snprintf(c->name, sizeof(c->name), "CPU core %u", n);
        snprintf(c->id, sizeof(c->id), "core%u", n);
        c->order = 100 + n;
        c->cpu = true;
        return;
    }
    for (u32 i = 0; i < ARRAY_COUNT(RAILS); ++i) {
        if (!strstr(ch, RAILS[i].tag)) continue;
        snprintf(c->name, sizeof(c->name), "%s", RAILS[i].name);
        snprintf(c->id, sizeof(c->id), "%s", RAILS[i].id);
        c->order = RAILS[i].order;
        c->cpu = RAILS[i].cpu;
        return;
    }
    snprintf(c->name, sizeof(c->name), "%s", c->raw);
    u32 n = 0;      // id: the channel name in lower case, other characters as '_'
    for (const char* p = ch; *p && n + 1 < sizeof(c->id); ++p)
        c->id[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ? *p : '_';
    c->id[n] = 0;
}

// Reads the device's metadata and adds its channels. False if it isn't a usable meter.
static bool add_device(EmiReader* r, HANDLE h, u8 dev) {
    DWORD got = 0;
    EMI_VERSION ver = {};
    EMI_METADATA_SIZE ms = {};
    if (!DeviceIoControl(h, IOCTL_EMI_GET_VERSION, nullptr, 0, &ver, sizeof(ver), &got, nullptr) ||
        !DeviceIoControl(h, IOCTL_EMI_GET_METADATA_SIZE, nullptr, 0, &ms, sizeof(ms), &got, nullptr) ||
        ms.MetadataSize == 0 || ms.MetadataSize > 16384)
        return false;
    alignas(8) u8 buf[16384];
    if (!DeviceIoControl(h, IOCTL_EMI_GET_METADATA, nullptr, 0, buf, ms.MetadataSize, &got, nullptr)) return false;
    r->version[dev] = ver.EmiVersion;
    char model[64];
    if (ver.EmiVersion == EMI_VERSION_V1) {
        const EMI_METADATA_V1* m = (const EMI_METADATA_V1*)buf;
        if (m->MeasurementUnit != EmiMeasurementUnitPicowattHours) return false;
        char hw[64];
        wide_name(m->HardwareModel, EMI_NAME_MAX, model, sizeof(model));
        wide_name(m->MeteredHardwareName, m->MeteredHardwareNameSize / sizeof(wchar_t), hw, sizeof(hw));
        add_channel(r, dev, 0, model, hw[0] ? hw : "Energy");
        r->channels_of[dev] = 1;
        return true;
    }
    if (ver.EmiVersion != EMI_VERSION_V2) return false;
    const EMI_METADATA_V2* m = (const EMI_METADATA_V2*)buf;
    wide_name(m->HardwareModel, EMI_NAME_MAX, model, sizeof(model));
    const EMI_CHANNEL_V2* c = m->Channels;
    const u8* end = buf + got;
    for (u16 i = 0; i < m->ChannelCount; ++i) {
        if ((const u8*)c + sizeof(EMI_CHANNEL_V2) > end) break;
        char name[64];
        wide_name(c->ChannelName, c->ChannelNameSize / sizeof(wchar_t), name, sizeof(name));
        if (c->MeasurementUnit == EmiMeasurementUnitPicowattHours) add_channel(r, dev, i, model, name);
        c = EMI_CHANNEL_V2_NEXT_CHANNEL(c);
    }
    r->channels_of[dev] = m->ChannelCount;
    return true;
}

u32 emi_open(EmiReader* r) {
    memset(r, 0, sizeof(*r));
    HDEVINFO set = SetupDiGetClassDevsW(&EMI_IFACE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        snprintf(r->status, sizeof(r->status), "Energy meters can't be listed (SetupAPI error %lu).", GetLastError());
        return 0;
    }
    SP_DEVICE_INTERFACE_DATA did = {};
    did.cbSize = sizeof(did);
    u32 found = 0, denied = 0;
    for (DWORD i = 0; r->device_count < EMI_MAX_DEVICES && SetupDiEnumDeviceInterfaces(set, nullptr, &EMI_IFACE, i, &did); ++i) {
        ++found;
        alignas(8) u8 buf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, sizeof(buf), nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { denied += GetLastError() == ERROR_ACCESS_DENIED; continue; }
        const u8 dev = (u8)r->device_count;
        if (!add_device(r, h, dev)) { CloseHandle(h); continue; }
        r->handle[dev] = h;
        ++r->device_count;
    }
    SetupDiDestroyDeviceInfoList(set);
    for (u32 i = 1; i < r->channel_count; ++i)      // insertion sort by order (devices enumerate in no useful order)
        for (u32 k = i; k > 0 && r->ch[k].order < r->ch[k - 1].order; --k) {
            const EmiChannel t = r->ch[k];
            r->ch[k] = r->ch[k - 1];
            r->ch[k - 1] = t;
        }
    if (!found) snprintf(r->status, sizeof(r->status), "This PC exposes no energy meter (EMI) device.");
    else if (!r->channel_count && denied)
        snprintf(r->status, sizeof(r->status), "%u energy meter(s) found, but opening them was denied (needs administrator rights).", found);
    else if (!r->channel_count)
        snprintf(r->status, sizeof(r->status), "%u energy meter(s) found, but none answered with power channels.", found);
    emi_read(r);
    return r->channel_count;
}

void emi_close(EmiReader* r) {
    for (u32 i = 0; i < r->device_count; ++i)
        if (r->handle[i]) CloseHandle((HANDLE)r->handle[i]);
    memset(r, 0, sizeof(*r));
}

void emi_read(EmiReader* r) {
    for (u32 d = 0; d < r->device_count; ++d) {
        alignas(8) EMI_CHANNEL_MEASUREMENT_DATA data[64];
        const u32 n = r->channels_of[d] < 64 ? r->channels_of[d] : 64;
        DWORD got = 0;
        const bool ok = DeviceIoControl((HANDLE)r->handle[d], IOCTL_EMI_GET_MEASUREMENT, nullptr, 0, data,
                                        n * sizeof(data[0]), &got, nullptr) != 0;
        for (u32 i = 0; i < r->channel_count; ++i) {
            EmiChannel* c = &r->ch[i];
            if (c->device != d) continue;
            if (!ok || (c->index + 1) * sizeof(data[0]) > got) { c->watts = NAN; continue; }
            const EMI_CHANNEL_MEASUREMENT_DATA* m = &data[c->index];
            if (c->time_100ns && m->AbsoluteTime > c->time_100ns && m->AbsoluteEnergy >= c->energy_pwh) {
                const f64 joules = (f64)(m->AbsoluteEnergy - c->energy_pwh) * 1e-12 * 3600.0;
                const f64 seconds = (f64)(m->AbsoluteTime - c->time_100ns) * 1e-7;
                c->watts = (f32)(joules / seconds);
            }
            c->energy_pwh = m->AbsoluteEnergy;
            c->time_100ns = m->AbsoluteTime;
        }
    }
}
