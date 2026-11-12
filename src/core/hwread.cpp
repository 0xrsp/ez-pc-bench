// Driverless CPU / GPU readers (see hwread.h).
#include "core/hwread.h"
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include "core/timer.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <pdh.h>
#include <powerbase.h>
#include <setupapi.h>
#include <initguid.h>     // defines the DEVPKEY_* / GUID_DEVCLASS_* below in this file
#include <devguid.h>
#include <devpkey.h>
#include <pciprop.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static_assert(sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION) * HW_MAX_LPS <= sizeof(CpuReader::prev), "prev too small");
constexpr u32 PDH_BUF = 512 * 1024;     // "GPU Engine" has one instance per process x engine (~400 here)

static u8* pdh_buf() { return (u8*)VirtualAlloc(nullptr, PDH_BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

// Formatted values of a wildcard counter into buf; returns the item count (0 on failure).
static DWORD counter_array(u8* buf, void* counter, PDH_FMT_COUNTERVALUE_ITEM_W** items) {
    if (!counter || !buf) return 0;
    DWORD size = PDH_BUF, count = 0;
    *items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf;
    if (PdhGetFormattedCounterArrayW((PDH_HCOUNTER)counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, *items) !=
        ERROR_SUCCESS)
        return 0;
    return count;
}

// ---- CPU ----------------------------------------------------------------------------------------------

void cpu_reader_open(CpuReader* r) {
    memset(r, 0, sizeof(*r));
    r->buf = pdh_buf();
    struct PROCESSOR_POWER_INFORMATION {        // ntpoapi.h layout, not in the user-mode SDK headers
        ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
    };
    PROCESSOR_POWER_INFORMATION ppi[HW_MAX_LPS] = {};
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi, sizeof(ppi)) == 0) r->base_mhz = (f32)ppi[0].MaxMhz;
    PDH_HQUERY q;
    PDH_HCOUNTER c;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    r->query = q;
    if (PdhAddEnglishCounterW(q, L"\\Processor Information(*)\\% Processor Performance", 0, &c) == ERROR_SUCCESS) r->perf = c;
    PdhCollectQueryData(q);         // rate counter: needs a first collection
    r->collect_t = timer_now();
}

void cpu_reader_close(CpuReader* r) {
    if (r->query) PdhCloseQuery((PDH_HQUERY)r->query);
    if (r->buf) VirtualFree(r->buf, 0, MEM_RELEASE);
    memset(r, 0, sizeof(*r));
}

// "0,5" -> 5 (group 0 only, matching the utilization query); -1 for "_Total" / "0,_Total" / other groups.
static i32 lp_index(const wchar_t* name) {
    if (wcsstr(name, L"_Total")) return -1;
    const wchar_t* comma = wcschr(name, L',');
    if (!comma || _wtoi(name) != 0) return -1;
    const i32 lp = _wtoi(comma + 1);
    return lp >= 0 && lp < (i32)HW_MAX_LPS ? lp : -1;
}

u32 cpu_reader_sample(CpuReader* r, f32* util, f32* mhz) {
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION cur[HW_MAX_LPS];
    ULONG len = 0;
    if (NtQuerySystemInformation(SystemProcessorPerformanceInformation, cur, sizeof(cur), &len) != 0) return 0;
    const u32 n = len / sizeof(cur[0]);
    // Rate counters over < 100 ms are noise (reads bunch up under load); keep accumulating until it's longer.
    const bool clock_ok = r->query && timer_now() - r->collect_t >= 0.1;
    if (clock_ok) {
        PdhCollectQueryData((PDH_HQUERY)r->query);
        r->collect_t = timer_now();
    }
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION* prev = (SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION*)r->prev;
    if (!r->have_prev || n == 0) {
        memcpy(prev, cur, sizeof(cur));
        r->have_prev = true;
        return 0;
    }
    for (u32 i = 0; i < n; ++i) {
        // KernelTime includes IdleTime.
        const f64 total = (f64)(cur[i].KernelTime.QuadPart - prev[i].KernelTime.QuadPart) +
                          (f64)(cur[i].UserTime.QuadPart - prev[i].UserTime.QuadPart);
        const f64 idle = (f64)(cur[i].IdleTime.QuadPart - prev[i].IdleTime.QuadPart);
        const f32 u = total > 0.0 ? (f32)(100.0 * (1.0 - idle / total)) : 0.0f;
        util[i] = u < 0.0f ? 0.0f : u > 100.0f ? 100.0f : u;
        mhz[i] = NAN;
    }
    memcpy(prev, cur, sizeof(cur));

    // Plausible range: up to 15 % over the SMBIOS max clock when the firmware reports one (5.1 GHz here; PDH reads
    // up to ~5.5 GHz on this CPU), else 3.5x nominal. Rates over short intervals under load spike far beyond it.
    const f64 max_pct = g_cpux.max_mhz > 0 && r->base_mhz > 0.0f ? g_cpux.max_mhz * 1.15 / r->base_mhz * 100.0 : 350.0;
    PDH_FMT_COUNTERVALUE_ITEM_W* items;
    const DWORD m = r->base_mhz > 0.0f && clock_ok ? counter_array(r->buf, r->perf, &items) : 0;
    for (DWORD i = 0; i < m; ++i) {
        const i32 lp = lp_index(items[i].szName);
        const f64 pct = items[i].FmtValue.doubleValue;
        if (lp < 0 || (u32)lp >= n || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        if (pct > 0.0 && pct < max_pct) mhz[lp] = (f32)(pct * r->base_mhz / 100.0);
    }
    return n;
}

// ---- GPU ----------------------------------------------------------------------------------------------

void gpu_reader_open(GpuReader* r, u32 only_gpu) {
    memset(r, 0, sizeof(*r));
    r->buf = pdh_buf();
    r->count = g_sys.gpu_count < 8 ? g_sys.gpu_count : 8;
    for (u32 i = 0; i < r->count; ++i) {
        const GpuInfo* g = &g_sys.gpus[i];
        r->luid_low[i] = g->luid_low;
        r->luid_high[i] = g->luid_high;
        r->warn_c[i] = 87.0f;
        if (g->software || (only_gpu < r->count && i != only_gpu)) continue;
        D3DKMT_OPENADAPTERFROMLUID oa = {};
        oa.AdapterLuid.LowPart = g->luid_low;
        oa.AdapterLuid.HighPart = g->luid_high;
        if (D3DKMTOpenAdapterFromLuid(&oa) != 0) continue;
        r->handle[i] = oa.hAdapter;
        D3DKMT_QUERYADAPTERINFO qi = {};
        qi.hAdapter = oa.hAdapter;
        D3DKMT_ADAPTER_PERFDATACAPS caps = {};
        qi.Type = KMTQAITYPE_ADAPTERPERFDATA_CAPS;
        qi.pPrivateDriverData = &caps;
        qi.PrivateDriverDataSize = sizeof(caps);
        if (D3DKMTQueryAdapterInfo(&qi) == 0 && caps.TemperatureWarning) r->warn_c[i] = caps.TemperatureWarning / 10.0f;
        D3DKMT_ADAPTERADDRESS addr = {};
        qi.Type = KMTQAITYPE_ADAPTERADDRESS;
        qi.pPrivateDriverData = &addr;
        qi.PrivateDriverDataSize = sizeof(addr);
        if (D3DKMTQueryAdapterInfo(&qi) == 0) {
            r->bus[i] = addr.BusNumber;
            r->address[i] = (addr.DeviceNumber << 16) | addr.FunctionNumber;
        } else {
            r->bus[i] = r->address[i] = UINT32_MAX;
        }
    }
    PDH_HQUERY q;
    PDH_HCOUNTER c;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    r->query = q;
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS) r->engine = c;
    if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c) == ERROR_SUCCESS) r->memory = c;
    PdhCollectQueryData(q);
}

void gpu_reader_close(GpuReader* r) {
    for (u32 i = 0; i < r->count; ++i)
        if (r->handle[i]) {
            D3DKMT_CLOSEADAPTER ca = { r->handle[i] };
            D3DKMTCloseAdapter(&ca);
        }
    if (r->query) PdhCloseQuery((PDH_HQUERY)r->query);
    if (r->buf) VirtualFree(r->buf, 0, MEM_RELEASE);
    memset(r, 0, sizeof(*r));
}

void gpu_reader_collect(GpuReader* r) {
    if (r->query) PdhCollectQueryData((PDH_HQUERY)r->query);
}

// Instance names carry "luid_0x<high>_0x<low>": true if they belong to this GPU.
static bool luid_match(const wchar_t* name, u32 low, i32 high) {
    const wchar_t* l = wcsstr(name, L"luid_0x");
    if (!l) return false;
    const wchar_t* low_s = wcsstr(l + 7, L"_0x");
    return low_s && wcstoul(l + 7, nullptr, 16) == (u32)high && wcstoul(low_s + 3, nullptr, 16) == low;
}

// PCIe link of the display device at (bus, device << 16 | function), from its SetupAPI properties.
static void read_link(u32 bus, u32 address, GpuReading* out) {
    if (bus == UINT32_MAX) return;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        DEVPROPTYPE type;
        UINT32 b = 0, a = 0;
        if (!SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_BusNumber, &type, (PBYTE)&b, sizeof(b), nullptr, 0) ||
            !SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_Address, &type, (PBYTE)&a, sizeof(a), nullptr, 0) ||
            b != bus || a != address)
            continue;
        UINT32 v = 0;
        // Link speed values are the PCIe register encoding: 1 = 2.5 GT/s (Gen 1) ... 5 = 32 GT/s (Gen 5).
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_PciDevice_CurrentLinkSpeed, &type, (PBYTE)&v, sizeof(v), nullptr, 0)) out->link_gen = (u8)v;
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_PciDevice_CurrentLinkWidth, &type, (PBYTE)&v, sizeof(v), nullptr, 0)) out->link_width = (u8)v;
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_PciDevice_MaxLinkSpeed, &type, (PBYTE)&v, sizeof(v), nullptr, 0)) out->link_gen_max = (u8)v;
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_PciDevice_MaxLinkWidth, &type, (PBYTE)&v, sizeof(v), nullptr, 0)) out->link_width_max = (u8)v;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

void gpu_reader_read(GpuReader* r, u32 gpu, GpuReading* out, bool with_link) {
    memset(out, 0, sizeof(*out));
    out->core_mhz = out->core_max_mhz = out->mem_mhz = out->mem_max_mhz = NAN;
    out->util = out->temp_c = out->fan_rpm = out->power_pct = out->vram_used_mib = NAN;
    if (gpu >= r->count || !r->handle[gpu]) return;
    D3DKMT_QUERYADAPTERINFO qi = {};
    qi.hAdapter = r->handle[gpu];
    D3DKMT_NODE_PERFDATA node = {};             // node 0 = the 3D / graphics engine
    qi.Type = KMTQAITYPE_NODEPERFDATA;
    qi.pPrivateDriverData = &node;
    qi.PrivateDriverDataSize = sizeof(node);
    if (D3DKMTQueryAdapterInfo(&qi) == 0) {
        if (node.Frequency) out->core_mhz = (f32)(node.Frequency / 1e6);
        if (node.MaxFrequency) out->core_max_mhz = (f32)(node.MaxFrequency / 1e6);
    }
    D3DKMT_ADAPTER_PERFDATA pd = {};
    qi.Type = KMTQAITYPE_ADAPTERPERFDATA;
    qi.pPrivateDriverData = &pd;
    qi.PrivateDriverDataSize = sizeof(pd);
    if (D3DKMTQueryAdapterInfo(&qi) == 0) {
        if (pd.MemoryFrequency) out->mem_mhz = (f32)(pd.MemoryFrequency / 1e6);
        if (pd.MaxMemoryFrequency) out->mem_max_mhz = (f32)(pd.MaxMemoryFrequency / 1e6);
        if (pd.Temperature) out->temp_c = pd.Temperature / 10.0f;
        if (pd.FanRPM) out->fan_rpm = (f32)pd.FanRPM;
        if (pd.Power) out->power_pct = pd.Power / 10.0f;
    }

    // Engine utilization: one instance per process x engine ("..._eng_<n>_engtype_<type>"); sum per engine
    // over processes and report the busiest engine.
    PDH_FMT_COUNTERVALUE_ITEM_W* items;
    DWORD n = counter_array(r->buf, r->engine, &items);
    f32 engines[64] = {};
    bool any = false;
    for (DWORD i = 0; i < n; ++i) {
        const wchar_t* e = wcsstr(items[i].szName, L"_eng_");
        if (!e || items[i].FmtValue.CStatus != ERROR_SUCCESS || !luid_match(items[i].szName, r->luid_low[gpu], r->luid_high[gpu]))
            continue;
        const u32 eng = (u32)_wtoi(e + 5);
        if (eng < 64) engines[eng] += (f32)items[i].FmtValue.doubleValue;
        any = true;
    }
    if (any) {
        f32 hi = 0.0f;
        for (u32 i = 0; i < 64; ++i) hi = engines[i] > hi ? engines[i] : hi;
        out->util = hi > 100.0f ? 100.0f : hi;
    }
    n = counter_array(r->buf, r->memory, &items);
    for (DWORD i = 0; i < n; ++i)
        if (items[i].FmtValue.CStatus == ERROR_SUCCESS && luid_match(items[i].szName, r->luid_low[gpu], r->luid_high[gpu]))
            out->vram_used_mib = (f32)(items[i].FmtValue.doubleValue / MiB) + (isnan(out->vram_used_mib) ? 0.0f : out->vram_used_mib);
    if (with_link) read_link(r->bus[gpu], r->address[gpu], out);
}
