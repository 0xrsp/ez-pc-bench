// Device lists (sysinfo_ext.h): PCI tree with PCIe links, chipset, USB tree, network adapters, audio endpoints and
// the driverless GPU details. One walk of the PnP device tree (cfgmgr32); no admin, no device is woken.
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <cfgmgr32.h>
#include <initguid.h>       // DEVPKEY_* / PKEY_* definitions in this file (selectany)
#include <devpkey.h>
#include <pciprop.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

DeviceInfo g_dev;

static void to_utf8(const wchar_t* w, char* out, u32 size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, nullptr, nullptr)) out[0] = 0;
}

const char* pci_class_name(u8 base, u8 sub) {
    switch (base) {
        case 0x01: return sub == 0x06 ? "SATA controller" : sub == 0x08 ? "NVMe controller" : "Storage controller";
        case 0x02: return "Network controller";
        case 0x03: return "Display controller";
        case 0x04: return sub == 0x03 ? "Audio device" : "Multimedia controller";
        case 0x05: return "Memory controller";
        case 0x06: return sub == 0x00 ? "Host bridge" : sub == 0x01 ? "ISA bridge" : sub == 0x04 ? "PCI bridge" : "Bridge";
        case 0x08: return "System peripheral";
        case 0x0C: return sub == 0x03 ? "USB controller" : sub == 0x05 ? "SMBus" : "Serial bus controller";
        case 0x0D: return "Wireless controller";
        case 0x10: return "Encryption controller";
        case 0x11: return "Signal processing";
        case 0x12: return "Processing accelerator";
        case 0x13: return "Instrumentation";
        default: return "Other";
    }
}

// ---- chipset table --------------------------------------------------------------------------------------------
// From the pci.ids database (pciutils, 2026-11-09): AMD root complexes (platform) and the device IDs that only
// exist on AMD desktop chipsets. Intel chipsets are named by their LPC / eSPI bridge, whose Windows name (from
// Intel's chipset INF) carries the model.

struct PciName { u16 device; const char* name; };
static const PciName AMD_ROOTS[] = {
    { 0x1122, "Krackan Point" }, { 0x1450, "Zen (Family 17h)" }, { 0x1480, "Matisse / Starship" },
    { 0x14A4, "Genoa / Bergamo" }, { 0x14B5, "Family 17h-19h" }, { 0x14D8, "Raphael / Granite Ridge" },
    { 0x14E8, "Phoenix / Hawk Point" }, { 0x1507, "Strix Point / Strix Halo" }, { 0x153A, "Turin" },
    { 0x15D0, "Raven Ridge / Picasso" }, { 0x15F8, "FireFlight" }, { 0x1630, "Renoir / Cezanne" },
    { 0x1645, "Van Gogh" },
};
static const PciName AMD_CHIPSETS[] = {
    { 0x43B0, "X370" }, { 0x43B1, "X399" }, { 0x43B4, "300 Series" }, { 0x43B5, "X370" }, { 0x43B6, "X399" },
    { 0x43B7, "300 Series" }, { 0x43B8, "A320" }, { 0x43B9, "X370" }, { 0x43BA, "X399" }, { 0x43BB, "300 Series" },
    { 0x43BC, "A320" }, { 0x43C6, "400 Series" }, { 0x43C7, "400 Series" }, { 0x43C8, "400 Series" },
    { 0x43D5, "400 Series" }, { 0x43E9, "500 Series" }, { 0x43EA, "500 Series" }, { 0x43EB, "500 Series" },
    { 0x43EC, "A520" }, { 0x43EE, "500 Series" }, { 0x43F4, "600 Series" }, { 0x43F5, "600 Series" },
    { 0x43F6, "600 Series" }, { 0x43F7, "600 Series" }, { 0x43F9, "B665" }, { 0x43FA, "A620" },
    { 0x43FC, "800 Series" }, { 0x43FD, "800 Series" },
};

static const char* table_name(const PciName* t, u32 n, u16 device) {
    for (u32 i = 0; i < n; ++i)
        if (t[i].device == device) return t[i].name;
    return nullptr;
}

static void chipset_query(DeviceInfo* d) {
    const PciDev* host = nullptr;
    const char* amd_chipset = nullptr;
    const PciDev* intel_lpc = nullptr;
    for (u32 i = 0; i < d->pci_count; ++i) {
        const PciDev* p = &d->pci[i];
        if (!host && p->bus == 0 && p->dev == 0 && p->fn == 0) host = p;
        if (p->vendor == 0x1022 && !amd_chipset) amd_chipset = table_name(AMD_CHIPSETS, ARRAY_COUNT(AMD_CHIPSETS), p->device);
        if (p->vendor == 0x8086 && p->base_class == 0x06 && p->sub_class == 0x01 && !intel_lpc) intel_lpc = p;
    }
    if (host) {
        const char* plat = host->vendor == 0x1022 ? table_name(AMD_ROOTS, ARRAY_COUNT(AMD_ROOTS), host->device) : nullptr;
        snprintf(d->host_bridge, sizeof(d->host_bridge), "%s (%04X:%04X)", plat ? plat : host->name, host->vendor, host->device);
        if (host->vendor == 0x1022)
            snprintf(d->chipset, sizeof(d->chipset), amd_chipset ? "AMD %s chipset" : "AMD %s SoC (no separate chipset)",
                     amd_chipset ? amd_chipset : plat ? plat : "Zen");
    }
    if (intel_lpc) {
        char name[96];
        snprintf(name, sizeof(name), "%s", intel_lpc->name);
        if (!strncmp(name, "Intel(R) ", 9)) memmove(name, name + 9, strlen(name + 9) + 1);
        snprintf(d->chipset, sizeof(d->chipset), "Intel %s (%04X:%04X)",
                 strstr(name, "standard") ? "PCH" : name, intel_lpc->vendor, intel_lpc->device);
    }
    if (!d->chipset[0] && host) snprintf(d->chipset, sizeof(d->chipset), "%s", d->host_bridge);
}

// ---- device tree walk --------------------------------------------------------------------------------------------

static bool prop(DEVINST dn, const DEVPROPKEY* key, void* buf, u32 size) {
    DEVPROPTYPE type;
    ULONG n = size;
    return CM_Get_DevNode_PropertyW(dn, key, &type, (PBYTE)buf, &n, 0) == CR_SUCCESS;
}

static void node_name(DEVINST dn, char* out, u32 size) {
    wchar_t w[256] = {};
    if (!prop(dn, &DEVPKEY_Device_FriendlyName, w, sizeof(w)) && !prop(dn, &DEVPKEY_Device_DeviceDesc, w, sizeof(w)))
        w[0] = 0;
    to_utf8(w, out, size);
    for (char* c = out; *c; ++c)
        if (*c == '|') *c = '/';    // the report uses '|' between table cells
}

static u32 hex_after(const wchar_t* s, const wchar_t* key, u32 digits) {
    const wchar_t* p = wcsstr(s, key);
    if (!p) return 0;
    wchar_t buf[9] = {};
    wcsncpy_s(buf, p + wcslen(key), digits);
    return (u32)wcstoul(buf, nullptr, 16);
}

struct WalkGpu { u32 bus, address; DEVINST dn; };
struct WalkState {
    DeviceInfo* d;
    WalkGpu gpus[8];
    u32 gpu_count;
};

static void add_pci(WalkState* w, DEVINST dn, const wchar_t* id, u8 depth) {
    DeviceInfo* d = w->d;
    if (d->pci_count >= DEV_MAX_PCI) return;
    PciDev* p = &d->pci[d->pci_count++];
    memset(p, 0, sizeof(*p));
    p->vendor = (u16)hex_after(id, L"VEN_", 4);
    p->device = (u16)hex_after(id, L"DEV_", 4);
    const u32 subsys = hex_after(id, L"SUBSYS_", 8);
    p->sub_device = (u16)(subsys >> 16);
    p->sub_vendor = (u16)subsys;
    p->rev = (u8)hex_after(id, L"REV_", 2);
    p->depth = depth;
    // Class code from the compatible IDs ("PCI\VEN_1022&DEV_14E8&CC_060000"); the PciDevice class properties
    // aren't set on every device.
    p->base_class = p->sub_class = p->prog_if = 0xFF;
    static wchar_t compat[4096];
    memset(compat, 0, 8);
    if (prop(dn, &DEVPKEY_Device_CompatibleIds, compat, sizeof(compat) - 4))
        for (const wchar_t* c = compat; *c; c += wcslen(c) + 1)
            if (const wchar_t* cc = wcsstr(c, L"CC_")) {
                const u32 len = (u32)wcsspn(cc + 3, L"0123456789ABCDEFabcdef");
                if (len < 4) continue;
                const u32 v = hex_after(c, L"CC_", len >= 6 ? 6 : 4);
                p->base_class = (u8)(len >= 6 ? v >> 16 : v >> 8);
                p->sub_class = (u8)(len >= 6 ? v >> 8 : v);
                if (len >= 6) p->prog_if = (u8)v;
                break;
            }
    u32 bus = 0xFFFFFFFF, addr = 0xFFFFFFFF, v = 0;
    prop(dn, &DEVPKEY_Device_BusNumber, &bus, 4);
    prop(dn, &DEVPKEY_Device_Address, &addr, 4);
    p->bus = bus;
    p->dev = addr == 0xFFFFFFFF ? addr : addr >> 16;
    p->fn = addr == 0xFFFFFFFF ? addr : addr & 0xFFFF;
    if (prop(dn, &DEVPKEY_PciDevice_CurrentLinkSpeed, &v, 4)) p->link_gen = (u8)v;
    if (prop(dn, &DEVPKEY_PciDevice_CurrentLinkWidth, &v, 4)) p->link_width = (u8)v;
    if (prop(dn, &DEVPKEY_PciDevice_MaxLinkSpeed, &v, 4)) p->link_gen_max = (u8)v;
    if (prop(dn, &DEVPKEY_PciDevice_MaxLinkWidth, &v, 4)) p->link_width_max = (u8)v;
    node_name(dn, p->name, sizeof(p->name));
    if (p->base_class == 0x03 && w->gpu_count < ARRAY_COUNT(w->gpus)) w->gpus[w->gpu_count++] = { bus, addr, dn };
    if (p->base_class == 0x0C && p->sub_class == 0x03 && d->usb_count < DEV_MAX_USB) {     // USB host controller
        UsbDev* u = &d->usb[d->usb_count++];
        memset(u, 0, sizeof(*u));
        u->controller = true;
        snprintf(u->name, sizeof(u->name), "%s", p->name);
    }
}

static void add_usb(DeviceInfo* d, DEVINST dn, const wchar_t* id, u8 depth) {
    if (d->usb_count >= DEV_MAX_USB || wcsstr(id, L"&MI_")) return;     // interfaces of a composite device
    UsbDev* u = &d->usb[d->usb_count++];
    memset(u, 0, sizeof(*u));
    u->vid = (u16)hex_after(id, L"VID_", 4);
    u->pid = (u16)hex_after(id, L"PID_", 4);
    u->depth = depth;
    node_name(dn, u->name, sizeof(u->name));
    u->hub = wcsstr(id, L"ROOT_HUB") || strstr(u->name, "Hub") || strstr(u->name, "hub");
}

// Depth-first walk. pci_depth / usb_depth count the PCI / USB ancestors of the node.
static void walk(WalkState* w, DEVINST dn, u8 pci_depth, u8 usb_depth, u32 level) {
    wchar_t id[MAX_DEVICE_ID_LEN];
    bool pci = false, usb = false;
    if (CM_Get_Device_IDW(dn, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
        ULONG status = 0, problem = 0;
        const bool present = CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS;
        pci = present && !wcsncmp(id, L"PCI\\", 4);
        usb = present && !wcsncmp(id, L"USB\\", 4);
        if (pci) add_pci(w, dn, id, pci_depth);
        if (usb) add_usb(w->d, dn, id, (u8)(usb_depth + 1));
    }
    if (level > 32) return;
    DEVINST child;
    if (CM_Get_Child(&child, dn, 0) != CR_SUCCESS) return;
    do {
        walk(w, child, (u8)(pci_depth + pci), (u8)(usb_depth + usb), level + 1);
    } while (CM_Get_Sibling(&child, child, 0) == CR_SUCCESS);
}

// ---- GPUs (driverless) ---------------------------------------------------------------------------------------

static void gpu_dev_query(DeviceInfo* d, const WalkState* w) {
    for (u32 i = 0; i < g_sys.gpu_count && i < ARRAY_COUNT(d->gpus); ++i) {
        GpuDevInfo* g = &d->gpus[i];
        memset(g, 0, sizeof(*g));
        D3DKMT_OPENADAPTERFROMLUID oa = {};
        oa.AdapterLuid.LowPart = g_sys.gpus[i].luid_low;
        oa.AdapterLuid.HighPart = g_sys.gpus[i].luid_high;
        if (D3DKMTOpenAdapterFromLuid(&oa) != 0) continue;
        D3DKMT_QUERYADAPTERINFO qi = {};
        qi.hAdapter = oa.hAdapter;
        D3DKMT_ADAPTERADDRESS addr = {};
        qi.Type = KMTQAITYPE_ADAPTERADDRESS;
        qi.pPrivateDriverData = &addr;
        qi.PrivateDriverDataSize = sizeof(addr);
        if (D3DKMTQueryAdapterInfo(&qi) == 0)
            for (u32 k = 0; k < w->gpu_count; ++k)
                if (w->gpus[k].bus == addr.BusNumber && w->gpus[k].address == ((addr.DeviceNumber << 16) | addr.FunctionNumber)) {
                    const DEVINST dn = w->gpus[k].dn;
                    FILETIME ft;
                    SYSTEMTIME st;
                    if (prop(dn, &DEVPKEY_Device_DriverDate, &ft, sizeof(ft)) && FileTimeToSystemTime(&ft, &st))
                        snprintf(g->driver_date, sizeof(g->driver_date), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
                    wchar_t prov[64] = {};
                    if (prop(dn, &DEVPKEY_Device_DriverProvider, prov, sizeof(prov))) to_utf8(prov, g->provider, sizeof(g->provider));
                    u32 v = 0;
                    if (prop(dn, &DEVPKEY_PciDevice_CurrentLinkSpeed, &v, 4)) g->link_gen = (u8)v;
                    if (prop(dn, &DEVPKEY_PciDevice_CurrentLinkWidth, &v, 4)) g->link_width = (u8)v;
                    if (prop(dn, &DEVPKEY_PciDevice_MaxLinkSpeed, &v, 4)) g->link_gen_max = (u8)v;
                    if (prop(dn, &DEVPKEY_PciDevice_MaxLinkWidth, &v, 4)) g->link_width_max = (u8)v;
                }
        static D3DKMT_ADAPTERREGISTRYINFO reg;
        memset(&reg, 0, sizeof(reg));
        qi.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
        qi.pPrivateDriverData = &reg;
        qi.PrivateDriverDataSize = sizeof(reg);
        if (D3DKMTQueryAdapterInfo(&qi) == 0) {
            to_utf8(reg.ChipType, g->chip, sizeof(g->chip));
            to_utf8(reg.BiosString, g->bios, sizeof(g->bios));
        }
        D3DKMT_DRIVERVERSION ver = {};
        qi.Type = KMTQAITYPE_DRIVERVERSION;
        qi.pPrivateDriverData = &ver;
        qi.PrivateDriverDataSize = sizeof(ver);
        if (D3DKMTQueryAdapterInfo(&qi) == 0 && ver >= 1000)
            snprintf(g->wddm, sizeof(g->wddm), "%u.%u", (u32)ver / 1000, (u32)ver % 1000 / 100);
        D3DKMT_ADAPTER_PERFDATACAPS caps = {};
        qi.Type = KMTQAITYPE_ADAPTERPERFDATA_CAPS;
        qi.pPrivateDriverData = &caps;
        qi.PrivateDriverDataSize = sizeof(caps);
        if (D3DKMTQueryAdapterInfo(&qi) == 0) {
            g->mem_bw_max = caps.MaxMemoryBandwidth;
            g->pcie_bw_max = caps.MaxPCIEBandwidth;
        }
        D3DKMT_ADAPTER_PERFDATA perf = {};
        qi.Type = KMTQAITYPE_ADAPTERPERFDATA;
        qi.pPrivateDriverData = &perf;
        qi.PrivateDriverDataSize = sizeof(perf);
        if (D3DKMTQueryAdapterInfo(&qi) == 0) g->mem_mhz_max = (u32)(perf.MaxMemoryFrequency / 1000000);
        D3DKMT_ADAPTERTYPE at = {};
        qi.Type = KMTQAITYPE_ADAPTERTYPE;
        qi.pPrivateDriverData = &at;
        qi.PrivateDriverDataSize = sizeof(at);
        g->integrated = g_sys.gpus[i].dedicated_vram <= 512 * MiB ||
                        (D3DKMTQueryAdapterInfo(&qi) == 0 && at.HybridIntegrated);
        D3DKMT_CLOSEADAPTER ca = { oa.hAdapter };
        D3DKMTCloseAdapter(&ca);
    }
}

// Tearing support (DXGI) and the Windows VRR / windowed-game optimizations setting.
static void present_query(DeviceInfo* d) {
    d->tearing = d->vrr_optimizations = -1;
    IDXGIFactory5* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) d->tearing = allow ? 1 : 0;
        f->Release();
    }
    wchar_t s[512] = {};
    DWORD size = sizeof(s);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", L"DirectXUserGlobalSettings",
                     RRF_RT_REG_SZ, nullptr, s, &size) == ERROR_SUCCESS)
        if (const wchar_t* p = wcsstr(s, L"VRROptimizeEnable=")) d->vrr_optimizations = p[18] == L'1' ? 1 : 0;
}

// ---- network / audio ------------------------------------------------------------------------------------------

static void net_query(DeviceInfo* d) {
    ULONG size = 32 * 1024;
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        IP_ADAPTER_ADDRESSES* list = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, size);
        if (!list) return;
        const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        const ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, list, &size);
        if (r == ERROR_SUCCESS) {
            for (const IP_ADAPTER_ADDRESSES* a = list; a && d->net_count < DEV_MAX_NET; a = a->Next) {
                if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
                NetAdapter* n = &d->net[d->net_count++];
                to_utf8(a->Description, n->name, sizeof(n->name));
                const char* type = a->IfType == IF_TYPE_ETHERNET_CSMACD ? "Ethernet" : a->IfType == IF_TYPE_IEEE80211 ? "Wi-Fi"
                                   : a->IfType == IF_TYPE_PPP ? "PPP" : a->IfType == IF_TYPE_WWANPP || a->IfType == IF_TYPE_WWANPP2 ? "Mobile"
                                   : "Other";
                snprintf(n->type, sizeof(n->type), "%s", type);
                n->up = a->OperStatus == IfOperStatusUp;
                n->link_bps = a->ReceiveLinkSpeed == (ULONG64)-1 ? 0 : a->ReceiveLinkSpeed;
            }
        }
        HeapFree(GetProcessHeap(), 0, list);
        if (r != ERROR_BUFFER_OVERFLOW) return;
    }
}

static void audio_query(DeviceInfo* d) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        for (u32 flow = 0; flow < 2; ++flow) {
            const EDataFlow df = flow ? eCapture : eRender;
            wchar_t* def_id = nullptr;
            IMMDevice* def = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(df, eConsole, &def))) { def->GetId(&def_id); def->Release(); }
            IMMDeviceCollection* col = nullptr;
            if (SUCCEEDED(en->EnumAudioEndpoints(df, DEVICE_STATE_ACTIVE, &col))) {
                UINT n = 0;
                col->GetCount(&n);
                for (UINT i = 0; i < n && d->audio_count < DEV_MAX_AUDIO; ++i) {
                    IMMDevice* dev = nullptr;
                    if (FAILED(col->Item(i, &dev))) continue;
                    AudioDev* a = &d->audio[d->audio_count++];
                    memset(a, 0, sizeof(*a));
                    a->capture = flow == 1;
                    wchar_t* id = nullptr;
                    if (SUCCEEDED(dev->GetId(&id))) {
                        a->is_default = def_id && !wcscmp(id, def_id);
                        CoTaskMemFree(id);
                    }
                    IPropertyStore* ps = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                        PROPVARIANT v;
                        PropVariantInit(&v);
                        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) to_utf8(v.pwszVal, a->name, sizeof(a->name));
                        PropVariantClear(&v);
                        if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB &&
                            v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                            const WAVEFORMATEX* wf = (const WAVEFORMATEX*)v.blob.pBlobData;
                            a->sample_rate = wf->nSamplesPerSec;
                            a->bits = wf->wBitsPerSample;
                            a->channels = wf->nChannels;
                        }
                        PropVariantClear(&v);
                        ps->Release();
                    }
                    for (char* c = a->name; *c; ++c)
                        if (*c == '|') *c = '/';
                    dev->Release();
                }
                col->Release();
            }
            if (def_id) CoTaskMemFree(def_id);
        }
        en->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

// ---- entry ----------------------------------------------------------------------------------------------------

void devices_refresh() {
    DeviceInfo* d = &g_dev;
    memset(d, 0, sizeof(*d));
    static WalkState w;
    memset(&w, 0, sizeof(w));
    w.d = d;
    DEVINST root;
    if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) walk(&w, root, 0, 0, 0);
    chipset_query(d);
    gpu_dev_query(d, &w);
    present_query(d);
    net_query(d);
    audio_query(d);
    displays_query(d);
    d->ready = true;
}

void devices_ensure() {
    if (!g_dev.ready) devices_refresh();
}
