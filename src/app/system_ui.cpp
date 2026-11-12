// System details window: the full system report (core/sysinfo_ext.h) with a section list on the left and the
// section's key / value lines and tables on the right. Device lists are gathered when the window opens; the
// Vulkan / D3D12 details only on "Query GPU APIs" (creating a device wakes a sleeping laptop dGPU).
#include "app/system_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include "imgui.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static SysReport s_rep;
static u32 s_section;               // item index of the selected section
static u32 s_built_ready;           // g_gpux_ready when the report was built
static std::atomic<bool> s_querying;
static char s_status[MAX_PATH + 64];

static void rebuild() {
    s_built_ready = g_gpux_ready.load(std::memory_order_acquire);
    sysreport_build(&s_rep);
    if (s_section >= s_rep.count || s_rep.items[s_section].kind != REP_SECTION) s_section = 0;
}

void system_ui_on_open() {
    rebuild();
}

static DWORD WINAPI query_thread(void*) {
    gpu_api_details_query_all();
    s_querying.store(false);
    return 0;
}

static void copy_report() {
    static char text[512 * 1024];
    sysreport_text(&s_rep, text, sizeof(text));
    ImGui::SetClipboardText(text);
    snprintf(s_status, sizeof(s_status), "Copied the report as text.");
}

static void save_report() {
    wchar_t dir[MAX_PATH], base[MAX_PATH + 64];
    ui_results_dir(dir);
    char stamp[32];
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", &t);
    swprintf(base, ARRAY_COUNT(base), L"%s\\ezbench_system_%S", dir, stamp);
    char path[MAX_PATH + 64];
    WideCharToMultiByte(CP_UTF8, 0, base, -1, path, sizeof(path), nullptr, nullptr);
    if (sysreport_save(base)) snprintf(s_status, sizeof(s_status), "Saved %s.txt and .toon.txt", path);
    else snprintf(s_status, sizeof(s_status), "Could not write %s.txt", path);
}

static void draw_toolbar() {
    if (theme_button("Refresh")) {
        devices_refresh();
        rebuild();
    }
    ImGui::SameLine();
    const bool busy = s_querying.load() || runner_is_running();
    ImGui::BeginDisabled(busy);
    if (theme_button(s_querying.load() ? "Querying GPUs..." : "Query GPU APIs")) {
        s_querying.store(true);
        if (HANDLE h = CreateThread(nullptr, 0, query_thread, nullptr, 0, nullptr)) CloseHandle(h);
        else s_querying.store(false);
    }
    ImGui::EndDisabled();
    theme_help("Creates a Vulkan instance and a D3D12 device on every GPU to read compute units, Vulkan version and "
               "extensions, D3D12 feature level and shader model, and Resizable BAR. This wakes a sleeping laptop "
               "GPU for a moment, so it only happens when you ask (and when a GPU test runs).");
    ImGui::SameLine();
    if (theme_button("Copy report")) copy_report();
    theme_help("Copies the whole report as plain text.");
    ImGui::SameLine();
    if (theme_button("Save report")) save_report();
    theme_help("Writes the report as plain text and as TOON into the results folder.");
    if (s_status[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s_status);
    }
}

static void draw_kv_block(u32 first, u32 end) {
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("kv", 2, flags)) return;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2.6f);
    for (u32 i = first; i < end; ++i) {
        const RepItem* it = &s_rep.items[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", rep_cell(&s_rep, it, 0));
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(rep_cell(&s_rep, it, 1));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
}

// A table in its own group box, titled with the table's name.
static void draw_table(u32 header, u32 end) {
    const RepItem* h = &s_rep.items[header];
    const u32 cols = h->cells - 1u < 16u ? h->cells - 1u : 16u;
    ImGui::PushID((int)header);
    theme_group_begin(rep_cell(&s_rep, h, 0));
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg;
    if (cols && ImGui::BeginTable("t", (int)cols, flags)) {
        for (u32 c = 0; c < cols; ++c) ImGui::TableSetupColumn(rep_cell(&s_rep, h, c + 1));
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        for (u32 i = header + 1; i < end; ++i) {
            ImGui::TableNextRow();
            for (u32 c = 0; c < cols; ++c) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rep_cell(&s_rep, &s_rep.items[i], c));
            }
        }
        ImGui::EndTable();
    }
    theme_group_end();
    ImGui::PopID();
}

static void draw_section(u32 sec) {
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.15f));
    ImGui::TextUnformatted(rep_cell(&s_rep, &s_rep.items[sec], 0));
    ImGui::PopFont();
    ImGui::Spacing();
    u32 i = sec + 1;
    bool first_kv = true;
    while (i < s_rep.count && s_rep.items[i].kind != REP_SECTION) {
        const RepKind k = s_rep.items[i].kind;
        u32 end = i + 1;
        if (k == REP_KV) {      // key / value lines: a group box ("General" for the section's first block)
            while (end < s_rep.count && s_rep.items[end].kind == REP_KV) ++end;
            ImGui::PushID((int)i);
            theme_group_begin(first_kv ? "General" : "");
            draw_kv_block(i, end);
            theme_group_end();
            ImGui::PopID();
            first_kv = false;
        } else if (k == REP_TABLE) {
            while (end < s_rep.count && s_rep.items[end].kind == REP_ROW) ++end;
            draw_table(i, end);
        } else if (k == REP_NOTE) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", rep_cell(&s_rep, &s_rep.items[i], 0));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
        i = end;
    }
}

void system_ui_draw() {
    if (g_gpux_ready.load(std::memory_order_acquire) != s_built_ready && !s_querying.load()) rebuild();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##system", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    draw_toolbar();
    ImGui::Spacing();
    const f32 list_w = ImGui::GetFontSize() * 13.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    theme_begin_panel("##sections", ImVec2(list_w, avail.y));
    for (u32 i = 0; i < s_rep.count; ++i) {
        if (s_rep.items[i].kind != REP_SECTION) continue;
        ImGui::PushID((int)i);
        const bool sel = s_section == i;
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Header, g_pal.secondary_container);
            ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_secondary_container);
        }
        if (ImGui::Selectable(rep_cell(&s_rep, &s_rep.items[i], 0), sel)) s_section = i;
        if (sel) ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    theme_end_panel();
    ImGui::SameLine();
    theme_begin_panel("##content", ImVec2(0.0f, avail.y));
    if (s_section < s_rep.count) draw_section(s_section);
    theme_end_panel();
    ImGui::End();
}
