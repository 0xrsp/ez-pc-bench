// Battery reader (see battery.h). Same enumeration as Microsoft's "Enumerating Battery Devices" sample.
#include "core/battery.h"
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <batclass.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// GUID_DEVCLASS_BATTERY / GUID_DEVICE_BATTERY (same value); local copy so this file needs no initguid.h.
static const GUID BATTERY_IFACE = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

static bool query_info(HANDLE h, u32 tag, BATTERY_QUERY_INFORMATION_LEVEL level, void* out, DWORD size) {
    BATTERY_QUERY_INFORMATION q = {};
    q.BatteryTag = tag;
    q.InformationLevel = level;
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof(q), out, size, &got, nullptr) && got > 0;
}

static void query_string(HANDLE h, u32 tag, BATTERY_QUERY_INFORMATION_LEVEL level, char* out, u32 size) {
    wchar_t w[128] = {};
    out[0] = 0;
    if (query_info(h, tag, level, w, sizeof(w) - sizeof(wchar_t)))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, nullptr, nullptr);
    // Trim trailing blanks (some firmware pads the strings).
    for (size_t n = strlen(out); n && (out[n - 1] == ' ' || out[n - 1] == '\t'); --n) out[n - 1] = 0;
}

static void read_static(HANDLE h, u32 tag, BatteryInfo* b) {
    memset(b, 0, sizeof(*b));
    b->wear_pct = NAN;
    query_string(h, tag, BatteryDeviceName, b->name, sizeof(b->name));
    query_string(h, tag, BatteryManufactureName, b->manufacturer, sizeof(b->manufacturer));
    query_string(h, tag, BatterySerialNumber, b->serial, sizeof(b->serial));
    BATTERY_INFORMATION bi = {};
    if (!query_info(h, tag, BatteryInformation, &bi, sizeof(bi))) return;
    memcpy(b->chemistry, bi.Chemistry, 4);
    for (i32 i = 3; i >= 0 && (b->chemistry[i] == 0 || b->chemistry[i] == ' '); --i) b->chemistry[i] = 0;
    b->relative = (bi.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
    if (!b->relative) {
        if (bi.DesignedCapacity && bi.DesignedCapacity != BATTERY_UNKNOWN_CAPACITY) b->design_wh = bi.DesignedCapacity / 1000.0f;
        if (bi.FullChargedCapacity && bi.FullChargedCapacity != BATTERY_UNKNOWN_CAPACITY) b->full_wh = bi.FullChargedCapacity / 1000.0f;
        if (b->design_wh > 0.0f && b->full_wh > 0.0f) b->wear_pct = 100.0f - b->full_wh / b->design_wh * 100.0f;
    }
    b->cycle_count = bi.CycleCount;
}

u32 battery_open(BatteryReader* r) {
    memset(r, 0, sizeof(*r));
    HDEVINFO set = SetupDiGetClassDevsW(&BATTERY_IFACE, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA did = {};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; r->count < BATTERY_MAX && SetupDiEnumDeviceInterfaces(set, nullptr, &BATTERY_IFACE, i, &did); ++i) {
        alignas(8) u8 buf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, sizeof(buf), nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        ULONG wait = 0, tag = 0;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag, sizeof(tag), &got, nullptr) || !tag) {
            CloseHandle(h);     // slot without a battery inserted
            continue;
        }
        const u32 k = r->count++;
        r->handle[k] = h;
        r->tag[k] = tag;
        read_static(h, tag, &r->info[k]);
        ULONG dk = 0;
        r->has_temp[k] = query_info(h, tag, BatteryTemperature, &dk, sizeof(dk)) && dk > 2000 && dk < 4000;
    }
    SetupDiDestroyDeviceInfoList(set);
    battery_read(r);
    return r->count;
}

void battery_close(BatteryReader* r) {
    for (u32 i = 0; i < r->count; ++i)
        if (r->handle[i]) CloseHandle((HANDLE)r->handle[i]);
    memset(r, 0, sizeof(*r));
}

bool battery_read(BatteryReader* r) {
    bool any = false;
    for (u32 i = 0; i < r->count; ++i) {
        BatteryInfo* b = &r->info[i];
        BatteryStatus* s = &b->status;
        s->state = 0;
        s->rate_w = s->voltage_v = s->capacity_wh = s->charge_pct = s->temp_c = NAN;
        BATTERY_WAIT_STATUS ws = {};
        ws.BatteryTag = r->tag[i];
        BATTERY_STATUS bs = {};
        DWORD got = 0;
        if (!DeviceIoControl((HANDLE)r->handle[i], IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof(ws), &bs, sizeof(bs), &got, nullptr))
            continue;
        any = true;
        s->state = bs.PowerState & (BAT_ONLINE | BAT_DISCHARGING | BAT_CHARGING | BAT_CRITICAL);
        if (bs.Voltage != BATTERY_UNKNOWN_VOLTAGE && bs.Voltage) s->voltage_v = bs.Voltage / 1000.0f;
        if (b->relative) {
            if (bs.Capacity != BATTERY_UNKNOWN_CAPACITY) s->charge_pct = (f32)bs.Capacity;
        } else {
            if ((ULONG)bs.Rate != BATTERY_UNKNOWN_RATE) s->rate_w = bs.Rate / 1000.0f;
            if (bs.Capacity != BATTERY_UNKNOWN_CAPACITY) {
                s->capacity_wh = bs.Capacity / 1000.0f;
                if (b->full_wh > 0.0f) s->charge_pct = s->capacity_wh / b->full_wh * 100.0f;
            }
        }
        ULONG dk = 0;     // tenths of a kelvin
        if (r->has_temp[i] && query_info((HANDLE)r->handle[i], r->tag[i], BatteryTemperature, &dk, sizeof(dk)) &&
            dk > 2000 && dk < 4000)
            s->temp_c = dk / 10.0f - 273.15f;
    }
    return any;
}

f32 battery_discharge_w(const BatteryReader* r) {
    f32 sum = NAN;
    for (u32 i = 0; i < r->count; ++i) {
        const BatteryStatus* s = &r->info[i].status;
        if (s->state & BAT_ONLINE) return NAN;
        if (!isnan(s->rate_w) && s->rate_w < 0.0f) sum = (isnan(sum) ? 0.0f : sum) - s->rate_w;
    }
    return sum;
}
