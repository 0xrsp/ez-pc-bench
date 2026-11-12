#include "core/sysinfo.h"
#include "core/elevation.h"
#include "core/sysinfo_ext.h"
#include <windows.h>
#include <dxgi1_6.h>
#include <powrprof.h>
#include <powersetting.h>
#include <winioctl.h>
#include <shobjidl.h>
#include <propsys.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

SysInfo g_sys;

// ---- CPU --------------------------------------------------------------------

static u64 xgetbv0() {
    u32 lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((u64)hi << 32) | lo;
}

static void trim(char* s) {
    char* start = s;
    while (*start == ' ') ++start;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
}

static void cpu_query(CpuInfo* c) {
    int r[4];
    __cpuid(r, 0);
    const int max_leaf = r[0];
    memcpy(c->vendor + 0, &r[1], 4);
    memcpy(c->vendor + 4, &r[3], 4);
    memcpy(c->vendor + 8, &r[2], 4);
    c->vendor[12] = 0;

    __cpuid(r, 0x80000000);
    if ((u32)r[0] >= 0x80000004) {
        for (int i = 0; i < 3; ++i) {
            __cpuid(r, 0x80000002 + i);
            memcpy(c->brand + i * 16, r, 16);
        }
        c->brand[48] = 0;
        trim(c->brand);
    }

    __cpuid(r, 1);
    const u32 eax = (u32)r[0], ecx1 = (u32)r[2];
    c->stepping = eax & 0xF;
    c->model = (eax >> 4) & 0xF;
    c->family = (eax >> 8) & 0xF;
    if (c->family == 0xF) c->family += (eax >> 20) & 0xFF;
    if (c->family >= 6) c->model += ((eax >> 16) & 0xF) << 4;

    const bool osxsave = (ecx1 >> 27) & 1;
    const u64 xcr0 = osxsave ? xgetbv0() : 0;
    const bool os_avx = (xcr0 & 0x6) == 0x6;
    const bool os_avx512 = (xcr0 & 0xE6) == 0xE6;

    c->sse42 = (ecx1 >> 20) & 1;
    c->aes = (ecx1 >> 25) & 1;
    c->avx = os_avx && ((ecx1 >> 28) & 1);
    c->fma = os_avx && ((ecx1 >> 12) & 1);

    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        const u32 ebx = (u32)r[1], ecx = (u32)r[2];
        c->avx2 = os_avx && ((ebx >> 5) & 1);
        c->sha = (ebx >> 29) & 1;
        c->vaes = os_avx && ((ecx >> 9) & 1);
        c->avx512f = os_avx512 && ((ebx >> 16) & 1);
        c->avx512dq = os_avx512 && ((ebx >> 17) & 1);
        c->avx512bw = os_avx512 && ((ebx >> 30) & 1);
        c->avx512vl = os_avx512 && ((ebx >> 31) & 1);
        c->avx512vnni = os_avx512 && ((ecx >> 11) & 1);
    }
}

static void cpu_topology(CpuInfo* c) {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &size);
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    if (GetLogicalProcessorInformationEx(RelationAll, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buf, &size)) {
        for (DWORD off = 0; off < size;) {
            auto* e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf + off);
            if (e->Relationship == RelationProcessorCore) {
                c->cores++;
                for (WORD g = 0; g < e->Processor.GroupCount; ++g)
                    c->threads += (u32)__popcnt64(e->Processor.GroupMask[g].Mask);
            } else if (e->Relationship == RelationCache) {
                const CACHE_RELATIONSHIP* cr = &e->Cache;
                const u32 kib = cr->CacheSize / 1024;
                if (cr->Level == 1 && cr->Type == CacheData) c->l1d_kib = kib;
                else if (cr->Level == 2) c->l2_kib = kib;
                else if (cr->Level == 3) c->l3_kib += kib;
            }
            off += e->Size;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// ---- RAM (raw SMBIOS, type 17) ------------------------------------------------

struct RawSmbios {
    u8 calling_method, major, minor, dmi_revision;
    u32 length;
    u8 data[1];
};

static const char* smbios_string(const u8* rec, u8 rec_len, const u8* end, u8 index) {
    if (index == 0) return "";
    const char* s = (const char*)rec + rec_len;
    for (u8 i = 1; i < index && (const u8*)s < end; ++i) s += strlen(s) + 1;
    return (const u8*)s < end ? s : "";
}

static const char* smbios_mem_type(u8 t) {
    switch (t) {
        case 0x12: return "DDR";    case 0x13: return "DDR2";   case 0x18: return "DDR3";
        case 0x1A: return "DDR4";   case 0x1B: return "LPDDR";  case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3"; case 0x1E: return "LPDDR4"; case 0x22: return "DDR5";
        case 0x23: return "LPDDR5"; default:   return "Unknown";
    }
}

static void copy_smbios(char* dst, u32 size, const u8* p, u8 len, const u8* end, u8 index) {
    snprintf(dst, size, "%s", smbios_string(p, len, end, index));
    trim(dst);
}

// Types 0 (BIOS), 1 (system), 2 (baseboard).
static void board_record(BoardInfo* b, const u8* p, u8 len, const u8* end) {
    const u8 type = p[0];
    if (type == 0 && len >= 0x09) {
        copy_smbios(b->bios_vendor, sizeof(b->bios_vendor), p, len, end, p[0x04]);
        copy_smbios(b->bios_version, sizeof(b->bios_version), p, len, end, p[0x05]);
        copy_smbios(b->bios_date, sizeof(b->bios_date), p, len, end, p[0x08]);
    } else if (type == 1 && len >= 0x08) {
        copy_smbios(b->sys_vendor, sizeof(b->sys_vendor), p, len, end, p[0x04]);
        copy_smbios(b->sys_product, sizeof(b->sys_product), p, len, end, p[0x05]);
    } else if (type == 2 && len >= 0x08) {
        copy_smbios(b->board_vendor, sizeof(b->board_vendor), p, len, end, p[0x04]);
        copy_smbios(b->board_product, sizeof(b->board_product), p, len, end, p[0x05]);
    }
}

static void smbios_query(RamInfo* ram, BoardInfo* board) {
    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) ram->total_bytes = ms.ullTotalPhys;
    ULONGLONG installed_kib = 0;
    if (GetPhysicallyInstalledSystemMemory(&installed_kib)) ram->installed_bytes = installed_kib * 1024;

    const DWORD sig = 'RSMB';
    UINT size = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (!size) return;
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    if (GetSystemFirmwareTable(sig, 0, buf, size) == size) {
        const RawSmbios* raw = (const RawSmbios*)buf;
        const u8* p = raw->data;
        const u8* end = raw->data + raw->length;
        while (p + 4 <= end) {
            const u8 type = p[0], len = p[1];
            if (len < 4) break;
            // Structure end: formatted area followed by a double-null-terminated string set.
            const u8* next = p + len;
            while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) ++next;
            next += 2;

            board_record(board, p, len, end);
            if (type == 17 && len >= 0x15 && ram->module_count < ARRAY_COUNT(ram->modules)) {
                u16 size_field = *(const u16*)(p + 0x0C);
                u64 bytes = 0;
                if (size_field == 0x7FFF && len >= 0x20) bytes = (u64)*(const u32*)(p + 0x1C) * MiB;
                else if (size_field != 0 && size_field != 0xFFFF)
                    bytes = (size_field & 0x8000) ? (u64)(size_field & 0x7FFF) * KiB : (u64)size_field * MiB;
                if (bytes) {
                    RamModule* m = &ram->modules[ram->module_count++];
                    m->size_bytes = bytes;
                    strncpy_s(m->type, smbios_mem_type(p[0x12]), _TRUNCATE);
                    // Bank locator ("P0 CHANNEL A") tells modules apart better than the device locator.
                    snprintf(m->locator, sizeof(m->locator), "%s %s", smbios_string(p, len, end, p[0x11]),
                             smbios_string(p, len, end, p[0x10]));
                    if (len >= 0x17) {
                        u16 sp = *(const u16*)(p + 0x15);
                        m->speed_mts = (sp == 0xFFFF && len >= 0x58) ? *(const u32*)(p + 0x54) : sp;
                    }
                    if (len >= 0x1B) {
                        strncpy_s(m->manufacturer, smbios_string(p, len, end, p[0x17]), _TRUNCATE);
                        strncpy_s(m->part, smbios_string(p, len, end, p[0x1A]), _TRUNCATE);
                    }
                    if (len >= 0x22) {
                        u16 cs = *(const u16*)(p + 0x20);
                        m->configured_mts = (cs == 0xFFFF && len >= 0x5C) ? *(const u32*)(p + 0x58) : cs;
                    }
                    trim(m->manufacturer);
                    trim(m->part);
                    trim(m->locator);
                }
            }
            if (type == 127) break;     // end-of-table
            p = next;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// ---- GPUs (DXGI, high-performance order) ---------------------------------------

static void gpu_query(SysInfo* s) {
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; s->gpu_count < ARRAY_COUNT(s->gpus) &&
         SUCCEEDED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
         ++i) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            GpuInfo* g = &s->gpus[s->gpu_count++];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g->name, sizeof(g->name), nullptr, nullptr);
            g->dedicated_vram = d.DedicatedVideoMemory;
            g->shared_mem = d.SharedSystemMemory;
            g->vendor_id = d.VendorId;
            g->device_id = d.DeviceId;
            g->luid_low = d.AdapterLuid.LowPart;
            g->luid_high = d.AdapterLuid.HighPart;
        }
        adapter->Release();
    }
    factory->Release();
}

u32 sysinfo_default_gpu() { return 0; }    // first adapter in DXGI high-performance order

// ---- short names ------------------------------------------------------------------------------------

static bool word_is(const char* w, const char* s) { return _stricmp(w, s) == 0; }

// Lowercase tokens of `name` joined by '-', dropping marketing words. `gpu` picks the GPU rules.
static void short_name(const char* name, char* out, u32 size, bool gpu) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", name ? name : "");
    static const char* const cuts[] = { " w/ ", " with ", " @", "Processor", " CPU" };   // everything after goes
    for (u32 i = 0; i < ARRAY_COUNT(cuts); ++i)
        if (char* p = strstr(buf, cuts[i])) *p = 0;

    char tokens[16][32];
    u32 n = 0;
    for (char* t = strtok(buf, " ,"); t && n < 16; t = strtok(nullptr, " ,")) {
        char w[32];
        u32 k = 0;
        for (const char* c = t; *c && k + 1 < sizeof(w); ++c) {
            if (*c == '(') { while (*c && *c != ')') ++c; if (!*c) break; continue; }     // (R) (TM)
            if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-') w[k++] = *c;
            else if (*c >= 'A' && *c <= 'Z') w[k++] = (char)(*c - 'A' + 'a');
        }
        w[k] = 0;
        if (!k) continue;
        const size_t len = strlen(w);
        static const char* const drop_cpu[] = { "amd", "intel", "core", "gen", "mobile" };
        static const char* const drop_gpu[] = { "nvidia", "amd", "intel", "geforce", "graphics", "gpu" };
        bool drop = false;
        const char* const* list = gpu ? drop_gpu : drop_cpu;
        const u32 count = gpu ? ARRAY_COUNT(drop_gpu) : ARRAY_COUNT(drop_cpu);
        for (u32 d = 0; d < count && !drop; ++d) drop = word_is(w, list[d]);
        if (!gpu && len > 2 && w[0] >= '0' && w[0] <= '9' &&            // "13th", "16-core"
            (!strcmp(w + len - 2, "th") || !strcmp(w + len - 2, "nd") || !strcmp(w + len - 2, "rd") ||
             !strcmp(w + len - 2, "st") || strstr(w, "-core")))
            drop = true;
        if (!drop) snprintf(tokens[n++], sizeof(tokens[0]), "%s", w);
    }

    out[0] = 0;
    u32 len = 0;
    for (u32 i = 0; i < n; ++i) {
        const char* t = tokens[i];
        char merged[64];
        if (!gpu && word_is(t, "ryzen") && i + 1 < n && tokens[i + 1][0] >= '0' && tokens[i + 1][0] <= '9') {
            snprintf(merged, sizeof(merged), "r%s", tokens[++i]);           // "Ryzen 7" -> "r7"
            t = merged;
        } else if (gpu && (word_is(t, "rtx") || word_is(t, "gtx") || word_is(t, "rx") || word_is(t, "gt")) && i + 1 < n) {
            snprintf(merged, sizeof(merged), "%s%s", t, tokens[++i]);       // "RTX 5060" -> "rtx5060"
            t = merged;
        } else if (gpu && word_is(t, "radeon") && i + 1 < n && word_is(tokens[i + 1], "rx")) {
            continue;                                                       // "Radeon RX 7600" -> "rx7600"
        }
        const int w = snprintf(out + len, size - len, "%s%s", len ? "-" : "", t);
        if (w < 0 || len + (u32)w >= size) break;
        len += (u32)w;
    }
    if (!out[0]) snprintf(out, size, "%s", gpu ? "gpu" : "cpu");
}

void cpu_short_name(const char* brand, char* out, u32 size) { short_name(brand, out, size, false); }
void gpu_short_name(const char* name, char* out, u32 size) { short_name(name, out, size, true); }

// ---- Power -------------------------------------------------------------------

// Windows 10/11 "power mode" overlays (Settings > Power > Power mode).
static const GUID OVERLAY_BEST_EFFICIENCY  = { 0x961cc777, 0x2547, 0x4f9d, { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a } };
static const GUID OVERLAY_BEST_PERFORMANCE = { 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } };
static const GUID SCHEME_HIGH_PERF         = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
static const GUID SCHEME_ULTIMATE          = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };

typedef DWORD(WINAPI* PowerGetEffectiveOverlaySchemeFn)(GUID*);

void sysinfo_refresh_power() {
    PowerInfo* pw = &g_sys.power;
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        pw->on_ac = ps.ACLineStatus == 1;
        pw->has_battery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
        pw->battery_percent = ps.BatteryLifePercent;
    }

    GUID* scheme = nullptr;
    pw->scheme_name[0] = 0;
    pw->scheme_high_perf = false;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS) {
        pw->scheme_high_perf = IsEqualGUID(*scheme, SCHEME_HIGH_PERF) || IsEqualGUID(*scheme, SCHEME_ULTIMATE);
        wchar_t name[64];
        DWORD bytes = sizeof(name);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, (UCHAR*)name, &bytes) == ERROR_SUCCESS)
            WideCharToMultiByte(CP_UTF8, 0, name, -1, pw->scheme_name, sizeof(pw->scheme_name), nullptr, nullptr);
        LocalFree(scheme);
    }

    // Not declared in the SDK headers; exported by powrprof.dll since Windows 10 1709.
    pw->mode = POWER_MODE_UNKNOWN;
    if (HMODULE m = GetModuleHandleW(L"powrprof.dll")) {
        auto fn = (PowerGetEffectiveOverlaySchemeFn)(void*)GetProcAddress(m, "PowerGetEffectiveOverlayScheme");
        GUID overlay = {};
        if (fn && fn(&overlay) == ERROR_SUCCESS) {
            if (IsEqualGUID(overlay, OVERLAY_BEST_PERFORMANCE)) pw->mode = POWER_MODE_PERFORMANCE;
            else if (IsEqualGUID(overlay, OVERLAY_BEST_EFFICIENCY)) pw->mode = POWER_MODE_EFFICIENCY;
            else pw->mode = POWER_MODE_BALANCED;
        }
    }
}

const char* power_mode_name(PowerMode m) {
    switch (m) {
        case POWER_MODE_EFFICIENCY: return "Best power efficiency";
        case POWER_MODE_BALANCED: return "Balanced";
        case POWER_MODE_PERFORMANCE: return "Best performance";
        default: return "Unknown";
    }
}

bool power_is_best_performance(const PowerInfo* p) {
    return p->mode == POWER_MODE_PERFORMANCE || p->scheme_high_perf;
}

// ---- Drives --------------------------------------------------------------------

#ifndef PERSISTENT_VOLUME_STATE_DEV_VOLUME
#define PERSISTENT_VOLUME_STATE_DEV_VOLUME 0x00002000
#endif

static const char* bus_name(u32 t) {
    switch (t) {
        case BusTypeScsi: return "SCSI";        case BusTypeAtapi: return "ATAPI";
        case BusTypeAta: return "ATA";          case BusType1394: return "1394";
        case BusTypeUsb: return "USB";          case BusTypeRAID: return "RAID";
        case BusTypeiScsi: return "iSCSI";      case BusTypeSas: return "SAS";
        case BusTypeSata: return "SATA";        case BusTypeSd: return "SD";
        case BusTypeMmc: return "MMC";          case BusTypeVirtual: return "Virtual";
        case BusTypeFileBackedVirtual: return "File-backed virtual";
        case BusTypeSpaces: return "Storage Spaces";
        case BusTypeNvme: return "NVMe";        case BusTypeSCM: return "SCM";
        case BusTypeUfs: return "UFS";          default: return "Unknown";
    }
}

static void storage_query(HANDLE vol, DriveInfo* d) {
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    u8 buf[1024] = {};
    DWORD got = 0;
    if (DeviceIoControl(vol, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, nullptr) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const STORAGE_DEVICE_DESCRIPTOR* dd = (const STORAGE_DEVICE_DESCRIPTOR*)buf;
        char vendor[32] = {}, product[48] = {};    // space padded in the descriptor
        if (dd->VendorIdOffset && dd->VendorIdOffset < got) strncpy_s(vendor, (const char*)buf + dd->VendorIdOffset, _TRUNCATE);
        if (dd->ProductIdOffset && dd->ProductIdOffset < got) strncpy_s(product, (const char*)buf + dd->ProductIdOffset, _TRUNCATE);
        trim(vendor);
        trim(product);
        snprintf(d->model, sizeof(d->model), "%s%s%s", vendor, vendor[0] ? " " : "", product);
        d->bus_type = (u32)dd->BusType;
        d->virtual_disk = dd->BusType == BusTypeVirtual || dd->BusType == BusTypeFileBackedVirtual;
    }
    strncpy_s(d->bus, bus_name(d->bus_type), _TRUNCATE);

    q.PropertyId = StorageAccessAlignmentProperty;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR al = {};
    if (DeviceIoControl(vol, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &al, sizeof(al), &got, nullptr) &&
        got >= offsetof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, BytesPerPhysicalSector) + sizeof(DWORD)) {
        d->logical_sector = al.BytesPerLogicalSector;
        d->physical_sector = al.BytesPerPhysicalSector;
    }

}

// Dev Drive flag. Needs a file system handle; the volume root directory works without admin.
static void dev_drive_query(const wchar_t* root, DriveInfo* d) {
    HANDLE h = CreateFileW(root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILE_FS_PERSISTENT_VOLUME_INFORMATION in = {}, out = {};
    in.FlagMask = PERSISTENT_VOLUME_STATE_DEV_VOLUME;
    in.Version = 1;
    DWORD got = 0;
    if (DeviceIoControl(h, FSCTL_QUERY_PERSISTENT_VOLUME_STATE, &in, sizeof(in), &out, sizeof(out), &got, nullptr))
        d->dev_drive = (out.VolumeFlags & PERSISTENT_VOLUME_STATE_DEV_VOLUME) != 0;
    CloseHandle(h);
}

bool drive_query(wchar_t letter, DriveInfo* d) {
    memset(d, 0, sizeof(*d));
    d->letter = (char)letter;
    d->bitlocker = -1;
    const wchar_t root[4] = { letter, L':', L'\\', 0 };
    const UINT type = GetDriveTypeW(root);
    if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) return false;

    wchar_t fs[16] = {};
    if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, ARRAY_COUNT(fs)))
        WideCharToMultiByte(CP_UTF8, 0, fs, -1, d->fs, sizeof(d->fs), nullptr, nullptr);
    d->refs = strcmp(d->fs, "ReFS") == 0;
    ULARGE_INTEGER free_user, total, free_all;
    if (GetDiskFreeSpaceExW(root, &free_user, &total, &free_all)) {
        d->total_bytes = total.QuadPart;
        d->free_bytes = free_user.QuadPart;
    }

    // Zero access rights are enough for storage property queries, no admin needed.
    const wchar_t dev[7] = { L'\\', L'\\', L'.', L'\\', letter, L':', 0 };
    HANDLE vol = CreateFileW(dev, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (vol == INVALID_HANDLE_VALUE)
        vol = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (vol != INVALID_HANDLE_VALUE) {
        storage_query(vol, d);
        CloseHandle(vol);
    } else {
        strncpy_s(d->bus, "Unknown", _TRUNCATE);
    }
    dev_drive_query(root, d);
    return true;
}

// Shell property System.Volume.BitLockerProtection: 1 = on, 2 = off, 3 = encrypting, 4 = decrypting,
// 5 = on (locked), 6 = suspended. Works without admin. Caller has COM initialized.
static void bitlocker_query(DriveInfo* d) {
    PROPERTYKEY key;
    if (FAILED(PSGetPropertyKeyFromName(L"System.Volume.BitLockerProtection", &key))) return;
    const wchar_t root[4] = { (wchar_t)d->letter, L':', L'\\', 0 };
    IShellItem2* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(root, nullptr, IID_PPV_ARGS(&item)))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(item->GetProperty(key, &v))) {
        if (v.vt == VT_I4) d->bitlocker = v.lVal;
        else if (v.vt == VT_UI4) d->bitlocker = (i32)v.ulVal;
        else if (v.vt == VT_I2) d->bitlocker = v.iVal;
        else if (v.vt == VT_UI1) d->bitlocker = v.bVal;
        d->bitlocker_on = d->bitlocker == 1 || d->bitlocker == 3 || d->bitlocker == 5;
    }
    PropVariantClear(&v);
    item->Release();
}

static void drives_query(SysInfo* s) {
    const DWORD mask = GetLogicalDrives();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (u32 i = 0; i < 26 && s->drive_count < ARRAY_COUNT(s->drives); ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t letter = (wchar_t)(L'A' + i);
        const wchar_t root[4] = { letter, L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        DriveInfo* d = &s->drives[s->drive_count];
        if (!drive_query(letter, d)) continue;
        if (SUCCEEDED(co)) bitlocker_query(d);
        s->drive_count++;
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

const DriveInfo* sysinfo_find_drive(wchar_t letter) {
    for (u32 i = 0; i < g_sys.drive_count; ++i)
        if (g_sys.drives[i].letter == (char)letter) return &g_sys.drives[i];
    return nullptr;
}

void drive_describe(const DriveInfo* d, char* buf, u32 size) {
    const char* bl = d->bitlocker < 0 ? "" : d->bitlocker_on ? ", BitLocker on" : ", BitLocker off";
    snprintf(buf, size, "%s, %s, %s%s, %.0f GiB (%.0f GiB free), sector %u/%u%s", d->model[0] ? d->model : "?", d->bus,
             d->fs, d->dev_drive ? " Dev Drive" : "", d->total_bytes / (f64)GiB, d->free_bytes / (f64)GiB,
             d->logical_sector, d->physical_sector, bl);
}

u32 drive_target_warnings(wchar_t letter, char lines[2][192]) {
    const DriveInfo* d = sysinfo_find_drive(letter);
    if (!d) return 0;
    u32 n = 0;
    if (d->virtual_disk || d->dev_drive) {
        const char* kind = d->virtual_disk && d->dev_drive ? "virtual disk (VHD) / Dev Drive"
                           : d->virtual_disk                ? "virtual disk (VHD)"
                                                            : "Dev Drive";
        snprintf(lines[n++], 192, "Disk target %c: is a %s (%s): results include virtualization / file system overhead.",
                 d->letter, kind, d->fs);
    }
    if (d->bitlocker_on)
        snprintf(lines[n++], 192, "BitLocker is on for disk target %c: (software encryption costs throughput and CPU).",
                 d->letter);
    return n;
}

// ---- OS ----------------------------------------------------------------------

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);

static void os_query(char* out, u32 size) {
    OSVERSIONINFOW v = { sizeof(v) };
    auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn && fn(&v) == 0)
        snprintf(out, size, "Windows %s (%lu.%lu.%lu)", v.dwBuildNumber >= 22000 ? "11" : "10",
                 v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    else
        snprintf(out, size, "Windows");
}

void sysinfo_init() {
    memset(&g_sys, 0, sizeof(g_sys));
    cpu_query(&g_sys.cpu);
    cpu_topology(&g_sys.cpu);
    cpu_details_query(&g_cpux);
    smbios_query(&g_sys.ram, &g_sys.board);
    gpu_query(&g_sys);
    gpu_details_query(&g_sys);
    winstate_query(&g_sys.win);
    drives_query(&g_sys);
    LoadLibraryW(L"powrprof.dll");
    sysinfo_refresh_power();
    os_query(g_sys.os, sizeof(g_sys.os));
    g_sys.elevated = is_elevated();
}
