// Windows state that moves benchmark numbers (FUTURE.md section 9): virtualization-based security and memory
// integrity (HVCI), Secure Boot, TPM, Game Mode, hardware-accelerated GPU scheduling, GPU driver versions.
// All of it readable without admin rights.
#include "core/sysinfo.h"
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_6.h>
#include <tbs.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

static i8 reg_dword(HKEY root, const wchar_t* key, const wchar_t* value, i8 if_missing) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return if_missing;
    return v ? 1 : 0;
}

// SystemIsolatedUserModeInformation (165): first byte = SecureKernelRunning:1, HvciEnabled:1, HvciStrictMode:1, ...
// Stable since Windows 10 1607 (used by System Informer / msinfo32's "Virtualization-based security" line).
static bool vbs_query(WinState* w) {
    u8 info[16] = {};
    ULONG len = 0;
    if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)165, info, sizeof(info), &len) != 0) return false;
    w->vbs = (info[0] & 1) ? 1 : 0;
    w->hvci = (info[0] & 2) ? 1 : 0;
    return true;
}

static void hvci_fallback(WinState* w) {
    SYSTEM_CODEINTEGRITY_INFORMATION ci = { sizeof(ci), 0 };
    ULONG len = 0;
    if (NtQuerySystemInformation(SystemCodeIntegrityInformation, &ci, sizeof(ci), &len) != 0) return;
    w->hvci = (ci.CodeIntegrityOptions & 0x400) ? 1 : 0;     // CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED
}

static void tpm_query(WinState* w) {
    TPM_DEVICE_INFO info = {};
    if (Tbsi_GetDeviceInfo(sizeof(info), &info) != TBS_SUCCESS) return;
    w->tpm = info.tpmVersion == TPM_VERSION_20 ? 20 : info.tpmVersion == TPM_VERSION_12 ? 12 : 0;
}

void winstate_query(WinState* w) {
    memset(w, 0, sizeof(*w));
    w->vbs = w->hvci = -1;
    int regs[4];
    __cpuid(regs, 1);
    w->hypervisor = (regs[2] >> 31) & 1;
    if (!vbs_query(w)) hvci_fallback(w);
    w->secure_boot = reg_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                               L"UEFISecureBootEnabled", -1);
    // Game Mode is on unless the user turned it off (the value is absent on a fresh install).
    w->game_mode = reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1);
    tpm_query(w);
}

// Hardware-accelerated GPU scheduling and the user-mode driver version for each GPU.
void gpu_details_query(SysInfo* s) {
    IDXGIFactory6* factory = nullptr;
    const bool have_factory = SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    for (u32 i = 0; i < s->gpu_count; ++i) {
        GpuInfo* g = &s->gpus[i];
        g->hags = -1;
        LUID luid;
        luid.LowPart = g->luid_low;
        luid.HighPart = g->luid_high;
        D3DKMT_OPENADAPTERFROMLUID oa = {};
        oa.AdapterLuid = luid;
        if (D3DKMTOpenAdapterFromLuid(&oa) == 0) {
            D3DKMT_WDDM_2_7_CAPS caps = {};
            D3DKMT_QUERYADAPTERINFO qi = {};
            qi.hAdapter = oa.hAdapter;
            qi.Type = KMTQAITYPE_WDDM_2_7_CAPS;
            qi.pPrivateDriverData = &caps;
            qi.PrivateDriverDataSize = sizeof(caps);
            if (D3DKMTQueryAdapterInfo(&qi) == 0 && caps.HwSchSupported) g->hags = caps.HwSchEnabled ? 1 : 0;
            D3DKMT_CLOSEADAPTER ca = { oa.hAdapter };
            D3DKMTCloseAdapter(&ca);
        }
        IDXGIAdapter1* adapter = nullptr;
        LARGE_INTEGER umd = {};
        if (have_factory && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                snprintf(g->driver, sizeof(g->driver), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                         HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            adapter->Release();
        }
    }
    if (have_factory) factory->Release();
}

static const char* on_off(i8 v) { return v < 0 ? "unknown" : v ? "on" : "off"; }

void winstate_describe(const WinState* w, char* buf, u32 size) {
    char tpm[16];
    snprintf(tpm, sizeof(tpm), w->tpm == 20 ? "2.0" : w->tpm == 12 ? "1.2" : "none");
    snprintf(buf, size, "VBS %s, memory integrity %s, Secure Boot %s, TPM %s, Game Mode %s", on_off(w->vbs),
             on_off(w->hvci), on_off(w->secure_boot), tpm, on_off(w->game_mode));
}

u32 winstate_warnings(const WinState* w, char lines[][192], u32 max) {
    u32 n = 0;
    if (w->hvci == 1 && n < max)
        snprintf(lines[n++], 192, "Memory integrity (HVCI) is on: it costs a few %% in some CPU, disk and game "
                                  "workloads. Compare runs only with the same setting.");
    if (w->game_mode == 0 && n < max)
        snprintf(lines[n++], 192, "Game Mode is off: background activity is less restricted during GPU tests.");
    return n;
}
