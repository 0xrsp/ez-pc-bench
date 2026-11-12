// Advanced sensors in the GUI (see adv_ui.h). Histories live here, fed from the core snapshots whenever a new
// sample arrived; the table matches the Sensors window's temperature tables.
#include "app/adv_ui.h"
#include "app/graph.h"
#include "app/theme.h"
#include "core/adv_sensors.h"
#include "core/pawnio_modules.h"
#include "core/sysinfo_spd.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <urlmon.h>
#include <wintrust.h>
#include <softpub.h>
#include <atomic>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

constexpr u32 HIST = 120;           // samples, one per second
constexpr u32 MAX_SERIES = 48;

struct Series {
    char name[48];
    f32 warn_c;
    f32 now, min, max;
    bool valid;
    f32 hist[HIST];
    u32 count;
};

static Series s_series[MAX_SERIES];
static u32 s_series_count;
static f64 s_last_cpu, s_last_mem;
static AdvCpu s_cpu;
static AdvMem s_mem;

void adv_ui_sensors_open() {
    s_series_count = 0;
    s_last_cpu = s_last_mem = 0.0;
    adv_start();
}

void adv_ui_sensors_close() { adv_stop(); }

static void feed(const char* name, f32 warn_c, f32 v) {
    Series* s = nullptr;
    for (u32 i = 0; i < s_series_count && !s; ++i)
        if (!strcmp(s_series[i].name, name)) s = &s_series[i];
    if (!s) {
        if (isnan(v) || s_series_count >= MAX_SERIES) return;      // rows appear once they have a value
        s = &s_series[s_series_count++];
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->warn_c = warn_c;
    }
    s->valid = !isnan(v);
    if (!s->valid) return;
    if (!s->count) s->min = s->max = v;
    s->now = v;
    s->min = v < s->min ? v : s->min;
    s->max = v > s->max ? v : s->max;
    if (s->count < HIST) s->hist[s->count++] = v;
    else {
        memmove(s->hist, s->hist + 1, (HIST - 1) * sizeof(f32));
        s->hist[HIST - 1] = v;
    }
}

static void update() {
    char name[48];
    if (adv_cpu_snapshot(&s_cpu) && s_cpu.time != s_last_cpu) {
        s_last_cpu = s_cpu.time;
        const bool amd = !isnan(s_cpu.tctl_c);
        feed(amd ? "CPU (Tdie)" : "CPU package", 95.0f, s_cpu.temp_c);
        if (amd && fabsf(s_cpu.tctl_c - s_cpu.temp_c) > 0.05f) feed("CPU (Tctl, fan control)", 95.0f, s_cpu.tctl_c);
        for (u32 i = 0; i < s_cpu.ccd_count; ++i) {
            snprintf(name, sizeof(name), "CCD %u", i + 1);
            feed(name, 90.0f, s_cpu.ccd_c[i]);
        }
        for (u32 i = 0; i < s_cpu.core_count; ++i) {
            snprintf(name, sizeof(name), "Core %u", i);
            feed(name, 95.0f, s_cpu.core_c[i]);
        }
    }
    if (adv_mem_snapshot(&s_mem) && s_mem.time != s_last_mem) {
        s_last_mem = s_mem.time;
        for (u32 i = 0; i < s_mem.count; ++i) {
            snprintf(name, sizeof(name), "DIMM at 0x%02X (%s)", s_mem.dimms[i].addr, s_mem.dimms[i].ddr5 ? "SPD hub" : "TSE2004");
            feed(name, 85.0f, s_mem.dimms[i].valid ? s_mem.dimms[i].temp_c : NAN);
        }
    }
}

static void temp_text(f32 c, ImVec4 col) {
    theme_push_numbers();
    ImGui::TextColored(col, "%.1f \xC2\xB0" "C", c);
    ImGui::PopFont();
}

static void draw_table() {
    if (!s_series_count) return;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##advtemps", 5, flags)) return;
    const f32 fs = ImGui::GetFontSize(), line = ImGui::GetTextLineHeight();
    ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, fs * 5.5f);
    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Last 2 minutes", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    for (u32 i = 0; i < s_series_count; ++i) {
        const Series* s = &s_series[i];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s->name);
        ImGui::TableNextColumn();
        if (s->valid) {
            const ImVec4 col = s->now >= s->warn_c ? g_pal.error : s->now >= s->warn_c - 10.0f ? g_pal.warn : g_pal.on_surface;
            temp_text(s->now, col);
        } else {
            ImGui::TextColored(g_pal.outline, "\xE2\x80\x94");
        }
        ImGui::TableNextColumn();
        if (s->count) temp_text(s->min, g_pal.on_surface_variant);
        ImGui::TableNextColumn();
        if (s->count) temp_text(s->max, g_pal.on_surface_variant);
        ImGui::TableNextColumn();
        if (s->count > 1) {
            Graph g;
            g.kind = GK_TEMP;
            g.unit = "\xC2\xB0""C";
            g.min_span = 10.0f;
            g.step_s = 1.0f;
            g.flags = GF_NO_CAPTION;
            GraphLine l;
            l.y = s->hist;
            l.n = s->count;
            graph_time("##hist", &g, &l, 1, ImVec2(0.0f, line * 2.0f));
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// "label value unit" pairs on one line, skipping NaN values; returns true if anything was drawn.
struct Field { const char* label; f32 v; const char* fmt; };
static bool field_line(const Field* f, u32 n) {
    char buf[256] = "";
    size_t len = 0;
    for (u32 i = 0; i < n; ++i) {
        if (isnan(f[i].v) || len + 2 >= sizeof(buf)) continue;
        if (len) len += snprintf(buf + len, sizeof(buf) - len, "  \xC2\xB7  ");
        len += snprintf(buf + len, sizeof(buf) - len, "%s ", f[i].label);
        len += snprintf(buf + len, sizeof(buf) - len, f[i].fmt, f[i].v);
    }
    if (!len) return false;
    theme_push_numbers();
    ImGui::TextUnformatted(buf);
    ImGui::PopFont();
    return true;
}

static void draw_values(const AdvInfo* info) {
    const AdvCpu* c = &s_cpu;
    const Field power[] = { { "Package", c->package_w, "%.1f W" }, { "cores", c->cores_w, "%.1f W" },
                            { "graphics", c->gfx_w, "%.1f W" }, { "SMU PPT", c->ppt_w, "%.1f W" } };
    const Field volts[] = { { "Vcore", c->vcore_v, "%.3f V" }, { "VSoC", c->vsoc_v, "%.3f V" } };
    const Field clocks[] = { { "FCLK", c->fclk_mhz, "%.0f MHz" }, { "UCLK", c->uclk_mhz, "%.0f MHz" },
                             { "MCLK", c->mclk_mhz, "%.0f MHz" } };
    field_line(power, ARRAY_COUNT(power));
    field_line(volts, ARRAY_COUNT(volts));
    field_line(clocks, ARRAY_COUNT(clocks));

    // Per-core PM table clocks and APERF/MPERF clocks: summary line, all values on hover.
    f32 sum = 0.0f, hi = NAN;
    u32 n = 0;
    for (u32 i = 0; i < c->lp_count; ++i)
        if (!isnan(c->lp_mhz[i]) && c->lp_busy[i] >= 50.0f) { sum += c->lp_mhz[i]; ++n; }
    for (u32 i = 0; i < c->lp_count; ++i)
        if (!isnan(c->lp_mhz[i]) && !(c->lp_mhz[i] <= hi)) hi = c->lp_mhz[i];
    if (c->lp_count) {
        theme_push_numbers();
        if (n) ImGui::Text("APERF/MPERF: busy cores %.2f GHz, highest %.2f GHz", sum / n / 1000.0f, hi / 1000.0f);
        else if (!isnan(hi)) ImGui::Text("APERF/MPERF: no core busy, highest %.2f GHz", hi / 1000.0f);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            theme_push_numbers();
            for (u32 i = 0; i < c->lp_count; ++i)
                if (!isnan(c->lp_mhz[i])) ImGui::Text("LP %2u  %5.0f MHz  %3.0f%% in C0", i, c->lp_mhz[i], c->lp_busy[i]);
            ImGui::PopFont();
            ImGui::EndTooltip();
        }
    } else if (s_last_cpu > 0.0) {
        ImGui::TextDisabled("Per-core clocks pause while a benchmark runs.");
    }
    if (!isnan(c->stretch_pct)) {
        ImGui::Text("Clock stretching %.1f%%", c->stretch_pct > 0.0f ? c->stretch_pct : 0.0f);
        theme_help("Busiest core's effective clock (APERF/MPERF) against the clock its current P-state asks for. A few "
                   "percent is normal measurement noise; more means the core is stretching its clock (voltage droop, "
                   "often from too aggressive an undervolt / Curve Optimizer).");
    }
    if (c->limits) {
        char lim[128] = "";
        static const char* const names[] = { "thermal", "PROCHOT", "power", "current", "cross-domain", "critical temperature" };
        for (u32 i = 0; i < ARRAY_COUNT(names); ++i)
            if (c->limits & (1u << i)) {
                const size_t len = strlen(lim);
                snprintf(lim + len, sizeof(lim) - len, "%s%s", len ? ", " : "", names[i]);
            }
        ImGui::TextColored(g_pal.warn, "Limiting now: %s", lim);
    }
    char t[192];
    if (memdetail_timings_line(t, sizeof(t))) ImGui::TextDisabled("Memory: %s", t);
    if (info->pm_table_version)
        ImGui::TextDisabled("SMU PM table 0x%06X%s", info->pm_table_version,
                            info->pm_table_mapped ? "" : " (layout not mapped yet: run the CLI with --adv-report to dump it)");
    if (info->smbus_note[0]) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("DIMM sensors: %s", info->smbus_note);
        ImGui::PopTextWrapPos();
    }
}

static void draw_modules_download();

void adv_ui_draw_sensors() {
    AdvInfo info;
    const AdvStatus st = adv_status(&info);
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted("Advanced sensors (PawnIO)");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    if (st == ADV_NO_MODULES) {
        ImGui::TextDisabled("The PawnIO driver is installed; the sensors also need its signed modules (PawnIO.Modules "
                            "%s from github.com, LGPL-2.1).", PAWNIO_MODULES_VERSION);
        draw_modules_download();
        ImGui::PopTextWrapPos();
        return;
    }
    if (st != ADV_ACTIVE) {
        ImGui::TextDisabled("%s. %s", adv_status_text(st), info.detail);
        ImGui::PopTextWrapPos();
        return;
    }
    update();
    if (info.detail[0]) ImGui::TextDisabled("%s", info.detail);
    if (!s_last_cpu) ImGui::TextDisabled("Reading\xE2\x80\xA6");
    draw_table();
    draw_values(&info);
    ImGui::PopTextWrapPos();
}

const char* adv_ui_settings_help() {
    static char help[1400];
    AdvInfo info;
    const AdvStatus st = adv_status(&info);
    snprintf(help, sizeof(help),
             "Per-core and package CPU temperatures, CPU power, voltages, memory temperatures, SPD and timings through "
             "PawnIO, a signed open-source driver used by LibreHardwareMonitor, FanControl and others (%s). EzPcBench "
             "doesn't ship it. \"Download & install\" fetches the official installer (PawnIO_setup.exe, latest release "
             "on github.com/namazso/PawnIO.Setup), checks its signature and starts it; you confirm the install there. "
             "It also downloads the signed PawnIO modules the sensors use (release %s of github.com/namazso/PawnIO.Modules, "
             "LGPL-2.1, checked against a pinned SHA-256) into %%LOCALAPPDATA%%\\ez-pc-bench\\pawnio. These buttons are "
             "the only time EzPcBench goes online. Without PawnIO everything else works as before.\n\n"
             "Status: %s. %s%s%s",
             ADV_PAWNIO_URL, PAWNIO_MODULES_VERSION, adv_status_text(st), info.detail, info.modules[0] ? "\nModules: " : "",
             info.modules);
    return help;
}

// ---- PawnIO installer and modules download --------------------------------------------------------------
// Only on an explicit click, on one worker thread (the UI thread only reads an atomic state):
// - "Download & install": download the official installer to %TEMP%\ezpcbench_pawnio\, require a valid
//   Authenticode signature from namazso (PawnIO's author), download the signed modules unless they are already
//   there (core/pawnio_modules.h), run the installer (it asks for elevation itself: requireAdministrator
//   manifest), wait for it to exit, delete it and detect PawnIO again;
// - "Download modules" (driver installed, modules missing): only the modules, then detect again, so the sensors
//   start without restarting the app.

enum InstallState : u8 { INST_IDLE, INST_DOWNLOADING, INST_MODULES, INST_RUNNING, INST_DONE, INST_FAILED };
static std::atomic<u8> s_inst;
static char s_inst_msg[512];        // written by the worker before it publishes INST_DONE / INST_FAILED
static HANDLE s_inst_thread;

static void inst_finish(InstallState st, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_inst_msg, sizeof(s_inst_msg), fmt, args);
    va_end(args);
    s_inst.store(st, std::memory_order_release);
}

// Valid, trusted Authenticode signature whose signer's organization is namazso.
static bool signed_by_pawnio_author(const wchar_t* path, char* why, u32 size) {
    WINTRUST_FILE_INFO file = { sizeof(file) };
    file.pcwszFilePath = path;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = { sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &file;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    const LONG trust = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    bool ok = trust == ERROR_SUCCESS;
    if (!ok) snprintf(why, size, "its signature isn't valid (0x%08lX)", (unsigned long)trust);
    if (ok) {
        wchar_t org[128] = L"";
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (cert)
            CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, (void*)szOID_ORGANIZATION_NAME, org, ARRAY_COUNT(org));
        ok = !_wcsicmp(org, L"namazso");
        if (!ok) snprintf(why, size, "it is signed by \"%ls\", not by PawnIO's author", org);
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return ok;
}

static DWORD WINAPI install_thread(void*) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    wcscat_s(dir, L"ezpcbench_pawnio");
    CreateDirectoryW(dir, nullptr);
    swprintf(path, MAX_PATH, L"%s\\PawnIO_setup.exe", dir);
    DeleteFileW(path);
    const HRESULT hr = URLDownloadToFileW(nullptr, ADV_PAWNIO_SETUP_URL, path, 0, nullptr);
    char why[160];
    if (FAILED(hr)) {
        inst_finish(INST_FAILED, "Download failed (error 0x%08lX). Check the internet connection, or get the installer "
                                 "from %s.", (unsigned long)hr, ADV_PAWNIO_URL);
    } else if (!signed_by_pawnio_author(path, why, sizeof(why))) {
        inst_finish(INST_FAILED, "Not started: the downloaded installer was rejected because %s. Get it from %s instead.",
                    why, ADV_PAWNIO_URL);
    } else {
        // Modules first, so the detection after the install finds them. A failure here doesn't stop the install:
        // the row then offers "Download modules" with the reason below it.
        char mod_msg[384] = "";
        bool mods = pawnio_modules_downloaded();
        if (!mods) {
            s_inst.store(INST_MODULES, std::memory_order_release);
            mods = pawnio_modules_download(mod_msg, sizeof(mod_msg));
        }
        s_inst.store(INST_RUNNING, std::memory_order_release);
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpFile = path;
        sei.lpDirectory = dir;
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            const DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) inst_finish(INST_FAILED, "Not installed: administrator rights were refused.");
            else inst_finish(INST_FAILED, "Couldn't start the installer (error %lu).", err);
        } else {
            DWORD code = 0;
            if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, INFINITE);
                GetExitCodeProcess(sei.hProcess, &code);
                CloseHandle(sei.hProcess);
            }
            adv_redetect();
            if (adv_status(nullptr) == ADV_NOT_INSTALLED)
                inst_finish(INST_DONE, code ? "The installer ended (code %lu) without installing PawnIO."
                                            : "The installer closed without installing PawnIO.", code);
            else if (!mods)
                inst_finish(INST_FAILED, "PawnIO installed, but its modules weren't downloaded: %s", mod_msg);
            else
                inst_finish(INST_DONE, "PawnIO installed.");
        }
    }
    DeleteFileW(path);
    RemoveDirectoryW(dir);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI modules_thread(void*) {
    char msg[384];
    const bool ok = pawnio_modules_download(msg, sizeof(msg));
    if (ok) adv_redetect();
    inst_finish(ok ? INST_DONE : INST_FAILED, "%s", msg);
    return 0;
}

static void job_start(bool modules_only) {
    if (s_inst_thread) {
        WaitForSingleObject(s_inst_thread, INFINITE);     // finished (state DONE / FAILED) before a retry
        CloseHandle(s_inst_thread);
    }
    s_inst_msg[0] = 0;
    s_inst.store(modules_only ? INST_MODULES : INST_DOWNLOADING, std::memory_order_release);
    s_inst_thread = CreateThread(nullptr, 0, modules_only ? modules_thread : install_thread, nullptr, 0, nullptr);
    if (!s_inst_thread) inst_finish(INST_FAILED, "Couldn't start the download (error %lu).", GetLastError());
}

// Progress text while the worker is busy; returns false (nothing drawn) when it isn't.
static bool draw_progress(u8 st) {
    const char* what = st == INST_DOWNLOADING ? "Downloading the PawnIO installer"
                     : st == INST_MODULES ? "Downloading the PawnIO modules"
                     : st == INST_RUNNING ? "Installer running" : nullptr;
    if (!what) return false;
    static const char* const dots[] = { "", ".", "..", "..." };
    ImGui::TextDisabled("%s%s", what, dots[(int)(ImGui::GetTime() * 2.0) % 4]);
    return true;
}

// The last job's outcome / error line, under the button.
static void draw_outcome(u8 st) {
    if (!s_inst_msg[0] || (st != INST_DONE && st != INST_FAILED)) return;
    ImGui::PushTextWrapPos(0.0f);
    if (st == INST_FAILED) ImGui::TextColored(g_pal.error, "%s", s_inst_msg);
    else ImGui::TextDisabled("%s", s_inst_msg);
    ImGui::PopTextWrapPos();
}

// The "Not installed" cell: Download & install, its progress, and the outcome / error line.
static void draw_install() {
    const u8 st = s_inst.load(std::memory_order_acquire);
    ImGui::AlignTextToFramePadding();
    if (draw_progress(st)) return;
    ImGui::TextUnformatted("Not installed");
    ImGui::SameLine();
    if (theme_button("Download & install")) job_start(false);     // what it does: the row's help (adv_ui_settings_help)
    draw_outcome(st);
}

// Driver installed, modules missing: one button (Settings row and Sensors window), progress, outcome.
static void draw_modules_download() {
    const u8 st = s_inst.load(std::memory_order_acquire);
    ImGui::AlignTextToFramePadding();
    if (draw_progress(st)) return;
    if (theme_button("Download modules")) job_start(true);
    draw_outcome(st);
}

void adv_ui_settings_control() {
    AdvInfo info;
    const AdvStatus st = adv_status(&info);
    if (st == ADV_NOT_INSTALLED) {
        draw_install();
        return;
    }
    bool on = adv_enabled();
    if (theme_checkbox("##adv", &on)) adv_set_enabled(on);
    ImGui::SameLine();
    if (st == ADV_NO_MODULES) {
        draw_modules_download();
        return;
    }
    ImGui::AlignTextToFramePadding();
    if (st == ADV_ACTIVE) ImGui::TextUnformatted("On");
    else ImGui::TextDisabled("%s", st == ADV_OFF ? "Off" : adv_status_text(st));
}
