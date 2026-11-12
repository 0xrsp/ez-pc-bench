// Disk health window (CrystalDiskInfo-style): a chip per drive, then for the selected drive the status,
// an info panel and tabs for the SMART table, self-tests and NVMe details.
// Read on open and on Refresh, never during disk tests.
#include "app/health_ui.h"
#include "app/theme.h"
#include "core/bench.h"
#include "core/disk_health.h"
#include "core/health_store.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include "imgui.h"
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum HealthTab : u8 { TAB_SMART, TAB_SELFTEST, TAB_NVME, TAB_COUNT };
static const char* const TAB_NAMES[TAB_COUNT] = { "S.M.A.R.T.", "Self-test", "NVMe details" };

struct HealthUi {
    DiskHealth disks[PHYSDISK_MAX];
    u32 count, sel;
    char read_at[32];
    bool scanned, options_loaded, hide_serial;
    HealthTab tab;
    char message[200];          // result of the last action
    f64 last_poll;
    i32 confirm_test;           // SelfTestKind waiting for a yes, -1 = none
    char report[256 * 1024];
};
static HealthUi s_ui;

static bool disk_test_running() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    return runner_is_running() && cur >= 0 && g_benches[cur].suite == SUITE_DISK;
}

static DiskHealth* selected() { return s_ui.sel < s_ui.count ? &s_ui.disks[s_ui.sel] : nullptr; }

static void scan() {
    s_ui.count = disk_health_scan(s_ui.disks, PHYSDISK_MAX);
    if (s_ui.sel >= s_ui.count) s_ui.sel = 0;
    // Start on the first real drive (virtual disks have nothing to show).
    if (s_ui.sel < s_ui.count && s_ui.disks[s_ui.sel].disk.virtual_disk)
        for (u32 i = 0; i < s_ui.count; ++i)
            if (!s_ui.disks[i].disk.virtual_disk) { s_ui.sel = i; break; }
    const time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    strftime(s_ui.read_at, sizeof(s_ui.read_at), "%H:%M:%S", &t);
    s_ui.scanned = true;
    s_ui.last_poll = timer_now();
}

void health_ui_on_open() {
    if (!s_ui.options_loaded) {
        s_ui.options_loaded = true;
        s_ui.hide_serial = health_ui_option("hide_serial", false);
        s_ui.confirm_test = -1;
    }
    if (!disk_test_running()) scan();
}

static ImVec4 status_color(HealthStatus s) {
    return s == HEALTH_GOOD ? g_pal.ok : s == HEALTH_CAUTION ? g_pal.warn : s == HEALTH_BAD ? g_pal.error : g_pal.outline;
}

static void bytes_text(char* buf, u32 size, u64 b) {
    if (b >= 1000000000000ull) snprintf(buf, size, "%.2f TB", b / 1e12);
    else snprintf(buf, size, "%.1f GB", b / 1e9);
}

static void degrees(char* buf, u32 size, f32 c) { snprintf(buf, size, "%.0f \xC2\xB0\x43", c); }     // \x43 = 'C'

// ---- actions ----------------------------------------------------------------------------------------

static void copy_report() {
    disk_health_report(s_ui.disks, s_ui.count, s_ui.hide_serial, s_ui.report, sizeof(s_ui.report));
    ImGui::SetClipboardText(s_ui.report);
    snprintf(s_ui.message, sizeof(s_ui.message), "Report copied to the clipboard.");
}

static void save_report() {
    wchar_t path[MAX_PATH];
    const time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    wcsftime(path, MAX_PATH, L"disk-health_%Y-%m-%d_%H%M.txt", &t);
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrTitle = L"Save disk health report";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool picked = GetSaveFileNameW(&ofn) != 0;
    if (SUCCEEDED(co)) CoUninitialize();
    if (!picked) return;
    const u32 len = disk_health_report(s_ui.disks, s_ui.count, s_ui.hide_serial, s_ui.report, sizeof(s_ui.report));
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, "\xEF\xBB\xBF", 3, &written, nullptr) &&
                    WriteFile(f, s_ui.report, len, &written, nullptr) && written == len;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    snprintf(s_ui.message, sizeof(s_ui.message), ok ? "Report saved." : "Could not write the report file.");
}

// ---- drive chips ------------------------------------------------------------------------------------

static void drive_chip(u32 i) {
    const DiskHealth* h = &s_ui.disks[i];
    char label[96], temp[24] = "";
    if (h->have_temp) degrees(temp, sizeof(temp), h->temp_c);
    snprintf(label, sizeof(label), "%s  %s", h->disk.letters[0] ? h->disk.letters : h->raid_member ? "RAID" : "Disk", temp);
    char sub[32];
    snprintf(sub, sizeof(sub), "%s", h->disk.virtual_disk ? "Virtual" : HEALTH_STATUS_NAMES[h->status]);
    const f32 fs = ImGui::GetFontSize(), pad = fs * 0.7f, dot = fs * 0.32f;
    const ImVec2 ls = ImGui::CalcTextSize(label), ss = ImGui::CalcTextSize(sub);
    const ImVec2 size(pad * 2.0f + dot * 2.0f + fs * 0.5f + (ls.x > ss.x ? ls.x : ss.x), fs * 2.6f);
    ImGui::PushID((int)i);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("chip", size);
    const bool sel = i == s_ui.sel, hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 bg = sel ? g_pal.secondary_container : hov ? g_pal.container_high : g_pal.card;
    const f32 r = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(bg), r);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(sel ? g_pal.secondary : g_pal.outline), r);
    const ImVec4 col = h->disk.virtual_disk ? g_pal.outline : status_color(h->status);
    dl->AddCircleFilled(ImVec2(p.x + pad + dot, p.y + size.y * 0.5f), dot, ImGui::GetColorU32(col));
    const f32 tx = p.x + pad + dot * 2.0f + fs * 0.5f;
    dl->AddText(ImVec2(tx, p.y + fs * 0.25f), ImGui::GetColorU32(sel ? g_pal.on_secondary_container : g_pal.on_surface), label);
    dl->AddText(ImVec2(tx, p.y + fs * 1.3f), ImGui::GetColorU32(col), sub);
    if (hov) {
        char tip[160];
        snprintf(tip, sizeof(tip), "%s\n%.0f GB  \xC2\xB7  %s", h->model[0] ? h->model : h->disk.model, h->disk.size_bytes / 1e9, h->disk.bus);
        ImGui::SetTooltip("%s", tip);
    }
    if (clicked) s_ui.sel = i;
    ImGui::PopID();
}

// ---- header + info ------------------------------------------------------------------------------------

static void status_box(const char* title, const char* value, ImVec4 col, f32 w) {
    const f32 fs = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(w, fs * 3.2f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 r = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.08f)), r);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.6f)), r);
    dl->AddText(ImVec2(p.x + fs * 0.7f, p.y + fs * 0.35f), ImGui::GetColorU32(g_pal.on_surface_variant), title);
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.3f));
    dl->AddText(ImVec2(p.x + fs * 0.7f, p.y + fs * 1.4f), ImGui::GetColorU32(col), value);
    ImGui::PopFont();
    ImGui::Dummy(size);
}

// One label / value pair of the info grid; `help` on hover of either.
static void cell(const char* label, const char* value, const char* help) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    if (help) theme_help(help);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value && value[0] ? value : "----");
    if (help) theme_help(help);
}

static void draw_header(DiskHealth* h) {
    char v[160];
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.25f));
    ImGui::Text("%s", h->model[0] ? h->model : h->disk.model);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%.1f GB  \xC2\xB7  Disk %u", h->disk.size_bytes / 1e9, h->disk.number);

    const f32 fs = ImGui::GetFontSize();
    if (h->life_left_pct >= 0) snprintf(v, sizeof(v), "%s  %d %%", HEALTH_STATUS_NAMES[h->status], h->life_left_pct);
    else snprintf(v, sizeof(v), "%s", HEALTH_STATUS_NAMES[h->status]);
    status_box("Health status", v, status_color(h->status), fs * 11.0f);
    theme_help("Good / Caution / Bad like CrystalDiskInfo. Bad: the drive itself reports a failure (SMART threshold "
               "exceeded, NVMe critical warning, spare below threshold, read-only). Caution: reallocated / pending / "
               "uncorrectable sectors, media errors or 10 % or less of the rated life left (CrystalDiskInfo's defaults). "
               "The percentage is the remaining rated life the drive estimates.");
    ImGui::SameLine();
    char t[32] = "----";
    ImVec4 tc = g_pal.on_surface;
    if (h->have_temp) {
        degrees(t, sizeof(t), h->temp_c);
        tc = h->warn_c > 0 && h->temp_c >= h->warn_c ? g_pal.error : h->temp_c >= 60 ? g_pal.warn : g_pal.ok;
    }
    status_box("Temperature", t, tc, fs * 8.0f);
    if (h->warn_c > 0) {
        char w[32], c[32];
        degrees(w, sizeof(w), h->warn_c);
        snprintf(v, sizeof(v), "The drive's own sensor. It throttles from its warning temperature (%s)", w);
        if (h->crit_c > 0) {
            degrees(c, sizeof(c), h->crit_c);
            snprintf(v + strlen(v), sizeof(v) - strlen(v), "; critical at %s", c);
        }
        snprintf(v + strlen(v), sizeof(v) - strlen(v), ".");
        theme_help(v);
    }
    ImGui::SameLine(0.0f, fs);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(h->status >= HEALTH_CAUTION ? status_color(h->status) : g_pal.on_surface, "%s", h->status_text);
    ImGui::PopTextWrapPos();
    if (h->source[0]) ImGui::TextDisabled("Read via %s%s", h->source, h->full_access ? "" : " (no administrator rights)");
    ImGui::EndGroup();
}

static void draw_info(DiskHealth* h) {
    char v[160], a[64];
    theme_group_begin("Drive information");
    if (!ImGui::BeginTable("info", 4, ImGuiTableFlags_SizingStretchProp)) { theme_group_end(); return; }
    ImGui::TableSetupColumn("l1", ImGuiTableColumnFlags_WidthStretch, 0.75f);
    ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("l2", ImGuiTableColumnFlags_WidthStretch, 0.75f);
    ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    cell("Firmware", h->firmware, nullptr);
    cell("Serial number", s_ui.hide_serial && h->serial[0] ? "********" : h->serial, "Hide it with \"Hide serial\" before taking screenshots.");
    cell("Interface", h->interface_name, h->loc.controller[0] ? h->loc.controller : nullptr);
    snprintf(v, sizeof(v), "%s | %s", h->transfer_cur[0] ? h->transfer_cur : "----", h->transfer_max[0] ? h->transfer_max : "----");
    cell("Transfer mode", v, "Current | maximum link. NVMe: PCIe generation and lanes of the drive's link; SATA: the negotiated "
                             "speed. A current link below the maximum costs sequential speed (wrong slot, chipset lanes, "
                             "power saving).");
    cell("Standard", h->standard, nullptr);
    if (h->rotation == 1) snprintf(v, sizeof(v), "---- (SSD)");
    else if (h->rotation) snprintf(v, sizeof(v), "%u RPM", h->rotation);
    else v[0] = 0;
    cell("Rotation rate", v, nullptr);
    if (h->have_power_cycles) snprintf(v, sizeof(v), "%llu", h->power_cycles);
    else v[0] = 0;
    cell("Power on count", v, "How many times the drive has been powered on.");
    if (h->have_power_on_hours) snprintf(v, sizeof(v), "%llu h  (%.0f days)", h->power_on_hours, h->power_on_hours / 24.0);
    else v[0] = 0;
    cell("Power on hours", v, "Hours powered on, not counting low-power idle states.");
    v[0] = 0;
    if (h->have_host_reads) bytes_text(v, sizeof(v), h->host_reads_bytes);
    cell("Host reads", v, "Data the computer read from the drive over its life (decimal units).");
    v[0] = 0;
    if (h->have_host_writes) bytes_text(v, sizeof(v), h->host_writes_bytes);
    cell("Host writes", v, "Data the computer wrote to the drive over its life. Endurance ratings (TBW) are in the same units.");
    v[0] = 0;
    if (h->have_nand_writes) bytes_text(v, sizeof(v), h->nand_writes_bytes);
    cell("NAND writes", v, "Data the controller wrote to flash, including garbage collection and wear levelling. Only some "
                           "drives report it (most NVMe drives don't); NAND / host writes is the write amplification.");
    v[0] = 0;
    if (h->buffer_bytes) snprintf(v, sizeof(v), "%u KB", (u32)(h->buffer_bytes / 1024));
    cell("Buffer size", v, "Cache size from IDENTIFY (an obsolete field: most drives report 0). DRAM-less NVMe drives borrow "
                           "host memory instead (HMB, listed under Features).");
    ImGui::EndTable();

    ImGui::TextDisabled("Features");
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(h->features[0] ? h->features : "----");
    ImGui::PopTextWrapPos();
    if (h->part_count) {
        char parts[1024] = "";
        for (u32 i = 0; i < h->part_count; ++i) {
            const DiskPartition* p = &h->parts[i];
            snprintf(v, sizeof(v), "#%u %s %.1f GB", p->number, p->type, p->size / 1e9);
            a[0] = 0;
            if (p->letter) snprintf(a, sizeof(a), " %c:", p->letter);
            const size_t len = strlen(parts);
            snprintf(parts + len, sizeof(parts) - len, "%s%s%s%s%s%s%s", i ? "   \xC2\xB7   " : "", v, a, p->fs[0] ? " " : "", p->fs,
                     p->label[0] ? " " : "", p->label);
        }
        ImGui::TextDisabled("Partitions (%s)", h->part_style);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(parts);
        ImGui::PopTextWrapPos();
    }
    theme_group_end();
}

// ---- SMART table ------------------------------------------------------------------------------------

static void draw_smart(DiskHealth* h) {
    if (!h->row_count) {
        ImGui::PushTextWrapPos(0.0f);
        if (h->disk.virtual_disk) ImGui::TextDisabled("Virtual disks have no SMART data.");
        else if (h->proto == HPROTO_ATA && !h->full_access)
            ImGui::TextDisabled("SATA SMART attributes need administrator rights (restart EzPcBench and accept the prompt). "
                                "What Windows hands out without them is shown above.");
        else ImGui::TextDisabled("This drive reports no SMART attributes to Windows.");
        ImGui::PopTextWrapPos();
        return;
    }
    const bool ata = h->proto == HPROTO_ATA;
    const u32 cols = ata ? 8 : 5;
    if (!ImGui::BeginTable("smart", (int)cols, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        return;
    const f32 fs = ImGui::GetFontSize();
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 0.8f);
    ImGui::TableSetupColumn("ID");
    ImGui::TableSetupColumn("Attribute", ImGuiTableColumnFlags_WidthStretch);
    if (ata) {
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Worst");
        ImGui::TableSetupColumn("Threshold");
    }
    ImGui::TableSetupColumn("Raw values");
    ImGui::TableSetupColumn("Decoded", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (u32 i = 0; i < h->row_count; ++i) {
        const SmartRow* r = &h->rows[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const f32 d = fs * 0.28f;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + d + 1.0f, p.y + ImGui::GetTextLineHeight() * 0.5f), d,
                                                    ImGui::GetColorU32(status_color(r->level)));
        ImGui::TableNextColumn();
        theme_push_numbers();
        ImGui::Text("%02X", r->id);
        ImGui::PopFont();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r->name);
        if (ata && r->flags) {
            char tip[160];
            snprintf(tip, sizeof(tip), "Flags 0x%04X: %s%s%s", r->flags, (r->flags & 1) ? "pre-failure (a value at or below the threshold "
                                                                                         "means the drive is failing)" : "advisory / old-age",
                     (r->flags & 2) ? ", updated online" : "", "");
            theme_help(tip);
        }
        theme_push_numbers();
        if (ata) {
            ImGui::TableNextColumn();
            ImGui::Text("%3u", r->current);
            ImGui::TableNextColumn();
            ImGui::Text("%3u", r->worst);
            ImGui::TableNextColumn();
            ImGui::Text("%3u", r->threshold);
        }
        ImGui::TableNextColumn();
        char raw[32] = {};
        for (u32 k = 0; k < 6; ++k) {       // most significant byte first
            const u32 idx = 5 - k;
            snprintf(raw + 2 * k, 3, "%02X", idx < r->raw_len ? r->raw[idx] : 0);
        }
        ImGui::TextUnformatted(raw);
        ImGui::PopFont();
        ImGui::TableNextColumn();
        if (r->level >= HEALTH_CAUTION) ImGui::TextColored(status_color(r->level), "%s", r->decoded);
        else ImGui::TextUnformatted(r->decoded);
    }
    ImGui::EndTable();
    if (ata && h->smart_status >= 0)
        ImGui::TextDisabled("SMART RETURN STATUS: %s", h->smart_status ? "threshold exceeded" : "OK");
    if (ata && h->ata_error_count) ImGui::TextColored(g_pal.warn, "ATA error log: %u errors logged over the drive's life", h->ata_error_count);
    ImGui::TextDisabled("Attribute names: %s table", SMART_VENDOR_NAMES[h->vendor]);
}

// ---- self-tests -------------------------------------------------------------------------------------------

static const char* const TEST_NAMES[] = { "Short", "Extended", "Conveyance", "Abort" };

static void draw_selftest(DiskHealth* h) {
    ImGui::PushTextWrapPos(0.0f);
    if (!h->can_self_test) {
        ImGui::TextDisabled("%s", h->proto == HPROTO_NONE || h->disk.virtual_disk ? "Self-tests need a drive with SMART / NVMe health data."
                                                                                  : "This drive doesn't report self-test support.");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("Self-tests run inside the drive: it reads (never writes) its own media and electronics while "
                        "you keep using the PC, a bit slower. They only start when you click a button below.");
    ImGui::PopTextWrapPos();
    const bool admin = g_sys.elevated;
    const bool running = h->test_progress >= 0;
    const bool busy = disk_test_running();
    struct { SelfTestKind kind; bool can; u16 minutes; } tests[] = {
        { SELFTEST_SHORT, h->can_short, h->minutes_short },
        { SELFTEST_EXTENDED, h->can_extended, h->minutes_extended },
        { SELFTEST_CONVEYANCE, h->can_conveyance, h->minutes_conveyance },
    };
    for (u32 i = 0; i < ARRAY_COUNT(tests); ++i) {
        if (!tests[i].can) continue;
        char label[64];
        if (tests[i].minutes) snprintf(label, sizeof(label), "%s test (~%u min)", TEST_NAMES[tests[i].kind], tests[i].minutes);
        else snprintf(label, sizeof(label), "%s test", TEST_NAMES[tests[i].kind]);
        ImGui::BeginDisabled(!admin || running || busy);
        if (theme_button(label)) s_ui.confirm_test = tests[i].kind;
        ImGui::EndDisabled();
        theme_help(!admin ? "Starting a self-test needs administrator rights."
                   : tests[i].kind == SELFTEST_SHORT    ? "Quick check of the electronics and a sample of the media (a minute or two)."
                   : tests[i].kind == SELFTEST_EXTENDED ? "Reads the whole surface / all flash. Takes long; the drive is slower meanwhile."
                                                         : "Checks for damage from transport (HDDs).");
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!admin || !running);
    if (theme_button("Abort")) {
        char err[160];
        if (disk_selftest_start(h, SELFTEST_ABORT, err, sizeof(err))) snprintf(s_ui.message, sizeof(s_ui.message), "Self-test aborted.");
        else snprintf(s_ui.message, sizeof(s_ui.message), "Abort failed: %s", err);
    }
    ImGui::EndDisabled();
    if (!admin) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(g_pal.warn, "Needs administrator rights.");
    }
    if (running) {
        ImGui::Text("%s test running: %d %%", h->test_running, h->test_progress);
        theme_progress(h->test_progress / 100.0f, ImGui::GetFontSize() * 0.35f, ImGui::GetFontSize() * 20.0f);
    }

    ImGui::Spacing();
    if (!h->test_count) {
        ImGui::TextDisabled("Self-test log: no tests recorded.");
        return;
    }
    theme_group_begin("Self-test log (newest first)");
    if (!ImGui::BeginTable("tests", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        theme_group_end();
        return;
    }
    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("At power-on hour", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("First failing LBA", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableHeadersRow();
    for (u32 i = 0; i < h->test_count; ++i) {
        const SelfTestEntry* e = &h->tests[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e->kind);
        ImGui::TableNextColumn();
        if (e->failed) ImGui::TextColored(g_pal.error, "%s", e->result);
        else ImGui::TextUnformatted(e->result);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", e->hours);
        ImGui::TableNextColumn();
        if (e->lba_valid) ImGui::Text("%llu", e->lba);
        else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
    theme_group_end();
}

static void draw_confirmations(DiskHealth* h) {
    if (s_ui.confirm_test >= 0) ImGui::OpenPopup("Start self-test?");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f));
    if (ImGui::BeginPopupModal("Start self-test?", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const SelfTestKind k = (SelfTestKind)(s_ui.confirm_test < 0 ? 0 : s_ui.confirm_test);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Start a %s self-test on %s?", k == SELFTEST_SHORT ? "short" : k == SELFTEST_EXTENDED ? "extended" : "conveyance", h->model);
        ImGui::TextDisabled("The drive tests itself in the background and doesn't change any data. It may be slower until "
                            "the test ends; you can abort it at any time. Disk benchmarks during the test give low numbers.");
        ImGui::PopTextWrapPos();
        if (theme_button_primary("Start")) {
            char err[160];
            if (disk_selftest_start(h, k, err, sizeof(err))) snprintf(s_ui.message, sizeof(s_ui.message), "%s self-test started.", TEST_NAMES[k]);
            else snprintf(s_ui.message, sizeof(s_ui.message), "Could not start the self-test: %s", err);
            s_ui.last_poll = timer_now();
            s_ui.confirm_test = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (theme_button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_ui.confirm_test = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---- NVMe details ---------------------------------------------------------------------------------------

static void hex_dump(const char* id, const u8* data, u32 size) {
    const f32 h = ImGui::GetTextLineHeightWithSpacing() * 14.0f;
    ImGui::BeginChild(id, ImVec2(0.0f, h), ImGuiChildFlags_Borders);
    theme_push_numbers();
    ImGuiListClipper clip;
    clip.Begin((int)(size / 16));
    while (clip.Step()) {
        for (int line = clip.DisplayStart; line < clip.DisplayEnd; ++line) {
            char text[128];
            int n = snprintf(text, sizeof(text), "%04X  ", line * 16);
            for (u32 k = 0; k < 16; ++k) n += snprintf(text + n, sizeof(text) - n, "%02X%s", data[line * 16 + k], k == 7 ? "  " : " ");
            n += snprintf(text + n, sizeof(text) - n, " ");
            for (u32 k = 0; k < 16; ++k) {
                const u8 c = data[line * 16 + k];
                text[n++] = c >= 0x20 && c < 0x7F ? (char)c : '.';
            }
            text[n] = 0;
            ImGui::TextUnformatted(text);
        }
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

static void kv(const char* k, const char* fmt, ...) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", k);
    ImGui::TableNextColumn();
    char v[160];
    va_list a;
    va_start(a, fmt);
    vsnprintf(v, sizeof(v), fmt, a);
    va_end(a);
    ImGui::TextUnformatted(v);
}

static void draw_nvme(DiskHealth* h) {
    if (h->proto != HPROTO_NVME || !h->have_nvme_id) {
        ImGui::TextDisabled("Only for NVMe drives.");
        return;
    }
    const u8* id = h->nvme_id;
    auto le16 = [](const u8* p) { return (u32)(p[0] | (p[1] << 8)); };
    auto le32 = [](const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); };
    theme_group_begin("Controller");
    if (ImGui::BeginTable("nvme", 4, ImGuiTableFlags_SizingStretchProp)) {
        kv("PCI vendor / subsystem", "%04X / %04X", le16(id), le16(id + 2));
        kv("IEEE OUI", "%02X%02X%02X", id[75], id[74], id[73]);
        kv("Controller id", "%u", le16(id + 78));
        kv("Namespaces", "%u", le32(id + 516));
        kv("Max data transfer", id[77] ? "%u KiB (with 4 KiB pages)" : "no limit", id[77] ? (1u << id[77]) * 4 : 0);
        kv("LBA size", "%u bytes", h->logical_sector ? h->logical_sector : 512);
        char w[32], c[32];
        degrees(w, sizeof(w), h->warn_c);
        degrees(c, sizeof(c), h->crit_c);
        kv("Warning / critical temp", "%s / %s", w, c);
        kv("Extended self-test", h->minutes_extended ? "%u min" : "not reported", h->minutes_extended);
        kv("RTD3 resume / entry", "%u / %u ms", le32(id + 84) / 1000, le32(id + 88) / 1000);
        kv("Host memory buffer", le32(id + 272) ? "%u MiB preferred, %u MiB min" : "not used", le32(id + 272) * 4 / 1024, le32(id + 276) * 4 / 1024);
        kv("Error log entries", "%u kept", id[262] + 1u);
        kv("Firmware slots", "%u%s", (id[260] >> 1) & 7, (id[260] & 1) ? " (slot 1 read-only)" : "");
        // Features (Get Features dword 0).
        if (h->feature_ok & (1u << 0x06)) kv("Volatile write cache", "%s", (h->feature[0x06] & 1) ? "enabled" : "disabled");
        if (h->feature_ok & (1u << 0x0C)) kv("APST (autonomous power states)", "%s", (h->feature[0x0C] & 1) ? "enabled" : "disabled");
        if (h->feature_ok & (1u << 0x02)) kv("Power state now", "PS%u%s", h->feature[0x02] & 0x1F, (h->feature[0x02] >> 5) & 7 ? " (workload hint)" : "");
        if (h->feature_ok & (1u << 0x04)) {
            const u32 k = h->feature[0x04] & 0xFFFF;
            kv("Temperature threshold", "%d \xC2\xB0\x43", (i32)k - 273);
        }
        if (h->feature_ok & (1u << 0x10)) {
            const u32 f = h->feature[0x10];
            if (f) kv("Host thermal management", "TMT1 %d \xC2\xB0\x43, TMT2 %d \xC2\xB0\x43", (i32)(f >> 16) - 273, (i32)(f & 0xFFFF) - 273);
            else kv("Host thermal management", "off");
        }
        if (h->feature_ok & (1u << 0x01)) kv("Arbitration burst", "%u", 1u << (h->feature[0x01] & 7));
        if (h->feature_ok & (1u << 0x08)) kv("Interrupt coalescing", "%u x 100 us, %u entries", (h->feature[0x08] >> 8) & 0xFF, (h->feature[0x08] & 0xFF) + 1);
        ImGui::EndTable();
    }
    theme_group_end();

    if (h->have_nvme_fw) {
        theme_group_begin("Firmware slots");
        const u32 slots = (id[260] >> 1) & 7;
        for (u32 s = 0; s < slots && s < 7; ++s) {
            char rev[9] = {};
            memcpy(rev, h->nvme_fw + 8 + 8 * s, 8);
            for (int k = 7; k >= 0 && (rev[k] == ' ' || rev[k] == 0); --k) rev[k] = 0;
            if (s) ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
            const bool active = (h->nvme_fw[0] & 7) == s + 1;
            if (rev[0]) ImGui::TextColored(active ? g_pal.primary : g_pal.on_surface, "%u: %s%s", s + 1, rev, active ? " (active)" : "");
            else ImGui::TextDisabled("%u: empty", s + 1);
        }
        theme_group_end();
    }

    theme_group_begin("Power states", 0.0f,
                      "From Identify Controller. Non-operational states are deep sleep; their exit latency is the delay "
                      "before the first I/O after idle (APST / Windows picks when to enter them).");
    if (ImGui::BeginTable("ps", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Max power");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Entry latency");
        ImGui::TableSetupColumn("Exit latency");
        ImGui::TableHeadersRow();
        for (u32 i = 0; i < h->power_state_count; ++i) {
            const NvmePowerState* p = &h->power_states[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("PS%u", i);
            ImGui::TableNextColumn(); ImGui::Text(p->max_watts >= 0.1f ? "%.2f W" : "%.4f W", p->max_watts);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p->non_operational ? "non-operational" : "operational");
            ImGui::TableNextColumn(); ImGui::Text("%u \xC2\xB5s", p->entry_us);
            ImGui::TableNextColumn(); ImGui::Text("%u \xC2\xB5s", p->exit_us);
        }
        ImGui::EndTable();
    }
    theme_group_end();

    theme_group_begin("Error information log");
    if (!h->error_count) {
        ImGui::TextDisabled("Empty.");
    } else {
        ImGui::TextDisabled("%u entries", h->error_count);
        if (ImGui::BeginTable("err", 6, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Error #");
            ImGui::TableSetupColumn("Queue");
            ImGui::TableSetupColumn("Command id");
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Namespace");
            ImGui::TableSetupColumn("LBA");
            ImGui::TableHeadersRow();
            for (u32 i = 0; i < h->error_count; ++i) {
                const NvmeErrorEntry* e = &h->errors[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%llu", e->count);
                ImGui::TableNextColumn(); ImGui::Text("%u", e->sqid);
                ImGui::TableNextColumn(); ImGui::Text("%u", e->cmdid);
                ImGui::TableNextColumn(); ImGui::Text("0x%04X", e->status);
                ImGui::TableNextColumn(); ImGui::Text("%u", e->nsid);
                ImGui::TableNextColumn(); ImGui::Text("%llu", e->lba);
            }
            ImGui::EndTable();
        }
    }
    theme_group_end();
    if (ImGui::CollapsingHeader("Identify Controller (raw)")) hex_dump("idc", h->nvme_id, 4096);
    if (h->have_nvme_ns && ImGui::CollapsingHeader("Identify Namespace 1 (raw)")) hex_dump("idn", h->nvme_ns, 4096);
    if (h->have_nvme_health && ImGui::CollapsingHeader("SMART / Health log (raw)")) hex_dump("hlog", h->nvme_health, 512);
}

// ---- frame ----------------------------------------------------------------------------------------------

void health_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##health", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const bool busy = disk_test_running();
    ImGui::BeginDisabled(busy);
    if (theme_button_primary("Refresh")) scan();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s_ui.count == 0);
    if (theme_button("Copy report")) copy_report();
    ImGui::SameLine();
    ImGui::BeginDisabled(runner_is_running());      // the file dialog would load shell extensions mid-benchmark
    if (theme_button("Save report...")) save_report();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    if (theme_checkbox("Hide serial", &s_ui.hide_serial)) health_set_ui_option("hide_serial", s_ui.hide_serial);
    theme_help("Replaces serial numbers with asterisks in this window and in copied / saved reports (for screenshots).");
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    ImGui::AlignTextToFramePadding();
    if (busy) ImGui::TextDisabled("Paused while a disk test runs.");
    else if (s_ui.message[0]) ImGui::TextDisabled("%s", s_ui.message);
    else if (s_ui.scanned) ImGui::TextDisabled("Read at %s", s_ui.read_at);
    bool missing = false;
    for (u32 i = 0; i < s_ui.count; ++i)
        missing |= !s_ui.disks[i].disk.virtual_disk && s_ui.disks[i].proto == HPROTO_ATA && !s_ui.disks[i].have_smart;
    if (!g_sys.elevated && missing) {
        ImGui::SameLine();
        ImGui::TextColored(g_pal.warn, "  Some drives need administrator rights for SMART data.");
    }

    // Drive chips.
    ImGui::Spacing();
    for (u32 i = 0; i < s_ui.count; ++i) {
        if (i) ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
        drive_chip(i);
    }
    if (s_ui.scanned && s_ui.count == 0) ImGui::TextDisabled("No physical drives found.");
    DiskHealth* h = selected();
    if (!h) { ImGui::End(); return; }

    // Poll a running self-test every 3 s.
    if (h->test_progress >= 0 && !busy && timer_now() - s_ui.last_poll > 3.0) {
        s_ui.last_poll = timer_now();
        disk_selftest_poll(h);
    }

    ImGui::Spacing();
    theme_begin_panel("drive", ImVec2(0, 0));
    draw_header(h);
    ImGui::Spacing();
    draw_info(h);
    ImGui::Spacing();
    for (u32 t = 0; t < TAB_COUNT; ++t) {
        if (t == TAB_NVME && h->proto != HPROTO_NVME) continue;
        if (t) ImGui::SameLine(0.0f, 4.0f);
        if (theme_tab(TAB_NAMES[t], s_ui.tab == t)) s_ui.tab = (HealthTab)t;
    }
    if (s_ui.tab == TAB_NVME && h->proto != HPROTO_NVME) s_ui.tab = TAB_SMART;
    ImGui::Spacing();
    switch (s_ui.tab) {
        case TAB_SMART: draw_smart(h); break;
        case TAB_SELFTEST: draw_selftest(h); break;
        case TAB_NVME: draw_nvme(h); break;
        default: break;
    }
    draw_confirmations(h);
    theme_end_panel();
    ImGui::End();
}
