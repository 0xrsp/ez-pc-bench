// Displays (sysinfo_ext.h): active paths from the display configuration API (mode, connector, adapter, HDR) and
// the monitor's EDID from its device registry key (name, native mode, refresh range, HDR / VRR blocks).
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include <windows.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void to_utf8(const wchar_t* w, char* out, u32 size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, nullptr, nullptr)) out[0] = 0;
}

static const char* connector_name(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t) {
    switch (t) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return "VGA";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return "DVI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return "LVDS (internal)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return "DisplayPort";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "eDP (internal)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return "UDI (internal)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return "Miracast";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return "Indirect (USB)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "Internal";
        default: return "Other";
    }
}

// ---- EDID ---------------------------------------------------------------------------------------------------

static void edid_parse(const u8* e, u32 size, DisplayInfo* d) {
    if (size < 128 || e[0] != 0x00 || e[1] != 0xFF) return;
    const u16 mfg = (u16)(e[8] << 8 | e[9]);
    d->maker[0] = (char)('A' - 1 + ((mfg >> 10) & 31));
    d->maker[1] = (char)('A' - 1 + ((mfg >> 5) & 31));
    d->maker[2] = (char)('A' - 1 + (mfg & 31));
    d->maker[3] = 0;
    d->product = (u16)(e[10] | e[11] << 8);
    d->edid_year = e[17] ? (u16)(1990 + e[17]) : 0;
    if (e[21] && e[22]) {
        const f32 w = e[21] / 2.54f, h = e[22] / 2.54f;
        d->size_in = sqrtf(w * w + h * h);
    }
    const bool continuous = (e[24] & 1) != 0;   // EDID 1.4: continuous frequency (variable refresh capable)
    bool have_name = false;
    for (u32 k = 0; k < 4; ++k) {
        const u8* b = e + 54 + 18 * k;
        if (b[0] || b[1]) {     // detailed timing; the first one is the preferred (native) mode
            if (d->native_w) continue;
            const u32 clock = (b[0] | b[1] << 8) * 10000u;
            const u32 ha = b[2] | (b[4] >> 4) << 8, hb = b[3] | (b[4] & 15) << 8;
            const u32 va = b[5] | (b[7] >> 4) << 8, vb = b[6] | (b[7] & 15) << 8;
            d->native_w = ha;
            d->native_h = va;
            if ((ha + hb) && (va + vb)) d->native_hz = (f32)clock / ((ha + hb) * (va + vb));
        } else if (b[3] == 0xFC || (b[3] == 0xFE && !have_name)) {     // monitor name; laptop panels: text string
            char name[16] = {};
            for (u32 i = 0; i < 13 && b[5 + i] != 0x0A; ++i) name[i] = (char)b[5 + i];
            for (size_t n = strlen(name); n && name[n - 1] == ' '; --n) name[n - 1] = 0;
            if (name[0]) snprintf(d->name, sizeof(d->name), "%s", name);
            have_name |= b[3] == 0xFC;      // the last 0xFE string is usually the panel part number
        } else if (b[3] == 0xFD) {      // range limits
            const u32 vmin = b[5] + ((b[4] & 2) ? 255 : 0), vmax = b[6] + ((b[4] & 1) ? 255 : 0);
            if (continuous && vmax > vmin + 10) { d->vrr_min_hz = (u16)vmin; d->vrr_max_hz = (u16)vmax; }
        }
    }
    // CTA-861 extensions: HDR static metadata block, AMD FreeSync and HDMI Forum VRR ranges.
    const u32 ext = e[126];
    for (u32 x = 1; x <= ext && (x + 1) * 128 <= size; ++x) {
        const u8* c = e + x * 128;
        if (c[0] != 0x02 || c[2] < 4) continue;
        for (u32 i = 4; i < c[2] && i < 127;) {
            const u8 tag = c[i] >> 5, len = c[i] & 31;
            const u8* p = c + i + 1;
            if (i + 1 + len > 127) break;
            if (tag == 7 && len >= 2 && p[0] == 6) d->hdr_supported = (p[1] & 4) ? 1 : 0;      // ST 2084 (PQ) EOTF
            if (tag == 3 && len >= 5 && p[0] == 0x1A && p[1] == 0x00 && p[2] == 0x00 && !d->vrr_max_hz) {   // AMD VSDB
                d->vrr_min_hz = p[4];
                d->vrr_max_hz = p[5];
            }
            if (tag == 3 && len >= 10 && p[0] == 0xD8 && p[1] == 0x5D && p[2] == 0xC4 && !d->vrr_max_hz) {   // HDMI Forum
                const u32 vmin = p[8] & 0x3F, vmax = ((p[8] & 0xC0u) << 2) | p[9];
                if (vmin && vmax > vmin) { d->vrr_min_hz = (u16)vmin; d->vrr_max_hz = (u16)vmax; }
            }
            i += 1 + len;
        }
    }
}

// EDID of the monitor behind a display configuration target (its device interface path).
static void edid_query(const wchar_t* interface_path, DisplayInfo* d) {
    wchar_t inst[MAX_DEVICE_ID_LEN] = {};
    DEVPROPTYPE type;
    ULONG size = sizeof(inst);
    if (CM_Get_Device_Interface_PropertyW(interface_path, &DEVPKEY_Device_InstanceId, &type, (PBYTE)inst, &size, 0) != CR_SUCCESS)
        return;
    DEVINST dn;
    if (CM_Locate_DevNodeW(&dn, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return;
    HKEY key;
    if (CM_Open_DevNode_Key(dn, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_HARDWARE) != CR_SUCCESS) return;
    u8 edid[1024];
    DWORD bytes = sizeof(edid);
    if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid, &bytes) == ERROR_SUCCESS) edid_parse(edid, bytes, d);
    RegCloseKey(key);
}

void displays_query(DeviceInfo* dev) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
    DISPLAYCONFIG_PATH_INFO* paths = (DISPLAYCONFIG_PATH_INFO*)HeapAlloc(GetProcessHeap(), 0, np * sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO* modes = (DISPLAYCONFIG_MODE_INFO*)HeapAlloc(GetProcessHeap(), 0, nm * sizeof(*modes));
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, nullptr) == ERROR_SUCCESS) {
        for (u32 i = 0; i < np && dev->display_count < DEV_MAX_DISPLAYS; ++i) {
            const DISPLAYCONFIG_PATH_INFO* p = &paths[i];
            DisplayInfo* d = &dev->displays[dev->display_count++];
            memset(d, 0, sizeof(*d));
            d->hdr_supported = d->hdr_on = -1;
            DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
            tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tn.header.size = sizeof(tn);
            tn.header.adapterId = p->targetInfo.adapterId;
            tn.header.id = p->targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
                to_utf8(tn.monitorFriendlyDeviceName, d->name, sizeof(d->name));
                snprintf(d->connector, sizeof(d->connector), "%s", connector_name(tn.outputTechnology));
                char windows_name[64];
                snprintf(windows_name, sizeof(windows_name), "%s", d->name);
                d->name[0] = 0;
                edid_query(tn.monitorDevicePath, d);
                if (!d->name[0]) snprintf(d->name, sizeof(d->name), "%s", windows_name[0] ? windows_name : "Display");
            }
            if (p->sourceInfo.modeInfoIdx < nm && modes[p->sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                d->width = modes[p->sourceInfo.modeInfoIdx].sourceMode.width;
                d->height = modes[p->sourceInfo.modeInfoIdx].sourceMode.height;
            }
            const DISPLAYCONFIG_RATIONAL rr = p->targetInfo.refreshRate;
            if (rr.Denominator) d->refresh_hz = (f32)rr.Numerator / rr.Denominator;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci = {};
            ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
            ci.header.size = sizeof(ci);
            ci.header.adapterId = p->targetInfo.adapterId;
            ci.header.id = p->targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS) {
                if (d->hdr_supported < 0) d->hdr_supported = ci.advancedColorSupported ? 1 : 0;   // no EDID HDR block
                d->hdr_on = ci.advancedColorEnabled && !ci.wideColorEnforced ? 1 : 0;   // not SDR auto color management
            }
            for (u32 g = 0; g < g_sys.gpu_count; ++g)
                if (g_sys.gpus[g].luid_low == p->targetInfo.adapterId.LowPart && g_sys.gpus[g].luid_high == p->targetInfo.adapterId.HighPart)
                    snprintf(d->adapter, sizeof(d->adapter), "%s", g_sys.gpus[g].name);
        }
    }
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (modes) HeapFree(GetProcessHeap(), 0, modes);
}
