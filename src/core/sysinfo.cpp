#include "core/sysinfo.h"
#include "core/elevation.h"
#include <windows.h>
#include <dxgi1_6.h>
#include <powrprof.h>
#include <powersetting.h>
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

static void ram_query(RamInfo* ram) {
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
        while (p + 4 <= end && ram->module_count < ARRAY_COUNT(ram->modules)) {
            const u8 type = p[0], len = p[1];
            if (len < 4) break;
            // Structure end: formatted area followed by a double-null-terminated string set.
            const u8* next = p + len;
            while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) ++next;
            next += 2;

            if (type == 17 && len >= 0x15) {
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
    ram_query(&g_sys.ram);
    gpu_query(&g_sys);
    LoadLibraryW(L"powrprof.dll");
    sysinfo_refresh_power();
    os_query(g_sys.os, sizeof(g_sys.os));
    g_sys.elevated = is_elevated();
}
