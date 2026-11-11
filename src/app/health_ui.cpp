// Disk health window: one block per physical drive with a status chip, identity line, reasons and a grid
// of health values (each with a hover explanation). Read on open and on Refresh, never during disk tests.
#include "app/health_ui.h"
#include "app/theme.h"
#include "core/bench.h"
#include "core/disk_health.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "imgui.h"
#include <stdio.h>
#include <time.h>

static DiskHealth s_disks[PHYSDISK_MAX];
static u32 s_count;
static char s_read_at[32];
static bool s_scanned;

static bool disk_test_running() {
    const i32 cur = g_run.current.load(std::memory_order_relaxed);
    return runner_is_running() && cur >= 0 && g_benches[cur].suite == SUITE_DISK;
}

static void scan() {
    s_count = disk_health_scan(s_disks, PHYSDISK_MAX);
    const time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    strftime(s_read_at, sizeof(s_read_at), "%H:%M:%S", &t);
    s_scanned = true;
}

void health_ui_on_open() {
    if (!disk_test_running()) scan();
}

static void format_bytes(char* buf, u32 size, u64 b) {
    if (b >= 1000000000000ull) snprintf(buf, size, "%.2f TB", b / 1e12);
    else snprintf(buf, size, "%.1f GB", b / 1e9);
}

// One label / value cell pair of the 4-column grid.
static void cell(const char* label, const char* value, const char* help, ImVec4 color) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    theme_help(help);
    ImGui::TableNextColumn();
    ImGui::TextColored(color, "%s", value);
    theme_help(help);
}

static void status_chip(HealthStatus s) {
    static const char* const names[] = { "Unknown", "Good", "Warning", "Critical" };
    const ImVec4 col = s == HEALTH_GOOD ? g_pal.ok : s == HEALTH_WARNING ? g_pal.warn : s == HEALTH_CRITICAL ? g_pal.error
                                                                                                          : g_pal.outline;
    const char* text = names[s];
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const f32 pad = ImGui::GetFontSize() * 0.6f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ts.x + pad * 2.0f, ts.y + pad * 0.5f);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y),
                                              ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.16f)), sz.y * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad, p.y + pad * 0.25f), ImGui::GetColorU32(col), text);
    ImGui::Dummy(sz);
}

static void draw_disk(const DiskHealth* h) {
    const PhysDisk* d = &h->disk;
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.1f));
    ImGui::Text("Disk %u  \xC2\xB7  %s", d->number, d->model);
    ImGui::PopFont();
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    status_chip(h->status);

    char ident[256];
    snprintf(ident, sizeof(ident), "%s  \xC2\xB7  %.0f GB%s%s%s%s%s%s", d->bus, d->size_bytes / 1e9,
             d->letters[0] ? "  \xC2\xB7  " : "", d->letters, d->firmware[0] ? "  \xC2\xB7  firmware " : "", d->firmware,
             d->serial[0] ? "  \xC2\xB7  serial " : "", d->serial);
    ImGui::TextDisabled("%s", ident);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(h->status_text);
    ImGui::PopTextWrapPos();

    if (!h->have_nvme && !h->have_temp) return;
    ImGui::Spacing();
    if (!ImGui::BeginTable("vals", 4, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("l1", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("l2", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    char v[64];
    const ImVec4 normal = g_pal.on_surface;
    if (h->have_nvme) {
        // Life used: value + bar of the remaining endurance.
        ImGui::TableNextColumn();
        const char* life_help = "How much of the drive's rated write endurance has been used, as estimated by the drive "
                                "itself. It can go past 100%; most drives keep working well beyond it, but it's a "
                                "sign to keep backups current.";
        ImGui::TextDisabled("Life used");
        theme_help(life_help);
        ImGui::TableNextColumn();
        snprintf(v, sizeof(v), "%u%%", h->used_pct);
        ImGui::TextColored(h->used_pct >= 90 ? g_pal.warn : normal, "%s", v);
        theme_help(life_help);
        ImGui::SameLine();
        const f32 left = h->used_pct >= 100 ? 0.0f : (100 - h->used_pct) / 100.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeight() * 0.35f);
        theme_progress(left, ImGui::GetFontSize() * 0.3f, ImGui::GetContentRegionAvail().x * 0.9f);

        snprintf(v, sizeof(v), "%u%% (threshold %u%%)", h->spare_pct, h->spare_threshold_pct);
        cell("Available spare", v, "Spare flash blocks left to replace worn-out ones, as a percentage. The drive warns "
                                   "when it falls below its threshold.",
             h->spare_pct < h->spare_threshold_pct ? g_pal.error : normal);
    }
    if (h->have_temp) {
        if (h->warn_c > 0.0f) snprintf(v, sizeof(v), "%.0f \xC2\xB0" "C (warns at %.0f)", h->temp_c, h->warn_c);
        else snprintf(v, sizeof(v), "%.0f \xC2\xB0" "C", h->temp_c);
        cell("Temperature", v, "The drive's own temperature sensor (NVMe: composite temperature). Drives slow down to "
                               "protect themselves when they reach their warning temperature.",
             h->warn_c > 0.0f && h->temp_c >= h->warn_c ? g_pal.error : normal);
    }
    if (h->have_nvme) {
        format_bytes(v, sizeof(v), h->data_written_bytes);
        cell("Data written", v, "Total data the computer has written to the drive over its life (decimal units). "
                                "Endurance ratings (TBW) are in the same units.", normal);
        format_bytes(v, sizeof(v), h->data_read_bytes);
        cell("Data read", v, "Total data the computer has read from the drive over its life.", normal);
        snprintf(v, sizeof(v), "%llu h (%.0f days)", h->power_on_hours, h->power_on_hours / 24.0);
        cell("Power-on time", v, "Hours the drive has been powered on, not counting low-power idle states.", normal);
        snprintf(v, sizeof(v), "%llu", h->power_cycles);
        cell("Power cycles", v, "How many times the drive has been powered on.", normal);
        snprintf(v, sizeof(v), "%llu", h->unsafe_shutdowns);
        cell("Unsafe shutdowns", v, "Power losses without a proper shutdown (battery ran out, forced power-off, crash). "
                                    "Usually harmless, but each one risks losing data still in the drive's cache.",
             normal);
        snprintf(v, sizeof(v), "%llu", h->media_errors);
        cell("Media errors", v, "Data the drive could not recover (uncorrectable ECC, checksum failures). Should be 0; "
                                "any other value is a reason to back up and keep an eye on the drive.",
             h->media_errors ? g_pal.error : normal);
        snprintf(v, sizeof(v), "%llu", h->error_log_entries);
        cell("Error log entries", v, "Entries in the drive's error log over its life. Often non-zero on healthy drives "
                                     "(e.g. unsupported commands sent by tools); media errors are what matter.",
             normal);
        snprintf(v, sizeof(v), "%u min / %u min", h->warn_temp_minutes, h->crit_temp_minutes);
        cell("Time too hot", v, "Minutes spent at or above the drive's warning temperature / critical temperature over "
                                "its life.",
             h->crit_temp_minutes ? g_pal.warn : normal);
    }
    ImGui::EndTable();
}

void health_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##health", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const bool busy = disk_test_running();
    ImGui::BeginDisabled(busy);
    if (theme_button("Refresh")) scan();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (busy) ImGui::TextDisabled("Paused while a disk test runs.");
    else if (s_scanned) ImGui::TextDisabled("Read at %s", s_read_at);
    // Many NVMe drivers answer health queries without admin rights; only point at elevation when a real
    // drive came back empty.
    bool missing = false;
    for (u32 i = 0; i < s_count; ++i)
        missing |= !s_disks[i].disk.virtual_disk && !s_disks[i].have_nvme && !s_disks[i].have_predict;
    if (!g_sys.elevated && missing) {
        ImGui::SameLine();
        ImGui::TextColored(g_pal.warn, "  Some drives need administrator rights to report health data.");
    }

    ImGui::BeginChild("##disks", ImVec2(0, 0));
    if (s_scanned && s_count == 0) ImGui::TextDisabled("No physical drives found.");
    for (u32 i = 0; i < s_count; ++i) {
        ImGui::PushID((int)i);
        ImGui::Spacing();
        // Card sized to its contents (theme_begin_panel fills the remaining height instead).
        const f32 pad = ImGui::GetFontSize();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.card);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad * 0.9f));
        ImGui::BeginChild("disk", ImVec2(-FLT_MIN, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        draw_disk(&s_disks[i]);
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}
