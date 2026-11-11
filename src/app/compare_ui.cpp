// Compare window: toolbar, filters and one wide table. Columns: metric, unit, then one per run; each run
// cell shows the raw value and its change vs the reference run (green = better, red = worse, > 1%).
#include "app/compare_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/baseline.h"
#include "core/compare.h"
#include "core/results.h"
#include "core/runner.h"
#include "imgui.h"
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct CompareUi {
    char status[300];
    u32 suite;                  // 0 = all, else index + 1 into the suite list
    ImGuiTextFilter filter;
    bool show_change = true;
    bool show_system = true;
};
static CompareUi s_cu;

static void status(const char* fmt, const char* arg) { snprintf(s_cu.status, sizeof(s_cu.status), fmt, arg); }

void compare_ui_add_path(const wchar_t* path) {
    char err[200];
    if (compare_add_file(path, err, sizeof(err))) {
        status("Added %s", g_cmp.runs[g_cmp.run_count - 1].label);
        return;
    }
    char name[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, name, sizeof(name), nullptr, nullptr);
    snprintf(s_cu.status, sizeof(s_cu.status), "Cannot load %s: %s", name, err);
}

static void add_current() {
    char err[200];
    if (compare_add_current(err, sizeof(err))) status("Added %s", g_cmp.runs[g_cmp.run_count - 1].label);
    else status("Cannot add the current run: %s", err);
}

void compare_ui_on_open() {
    char err[200];
    if (g_cmp.run_count == 0 && compare_add_current(err, sizeof(err))) status("Added %s", g_cmp.runs[0].label);
}

static void add_files_dialog() {
    static wchar_t buf[32 * 1024];
    wchar_t dir[MAX_PATH];
    ui_results_dir(dir);
    buf[0] = 0;
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
    ofn.lpstrFilter = L"ez-pc-bench results (*.toon.txt)\0*.toon.txt\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAY_COUNT(buf);
    ofn.lpstrInitialDir = dir;
    ofn.lpstrTitle = L"Add results to compare";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool picked = GetOpenFileNameW(&ofn) != 0;
    if (SUCCEEDED(co)) CoUninitialize();
    if (!picked) return;
    // One file: full path. Several: directory, then file names, each null-terminated, ending with an empty one.
    const wchar_t* first = buf + wcslen(buf) + 1;
    if (!*first) { compare_ui_add_path(buf); return; }
    for (const wchar_t* f = first; *f; f += wcslen(f) + 1) {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\%s", buf, f);
        compare_ui_add_path(path);
    }
}

static char s_copy[512 * 1024];
static u32 s_copy_len;

#if defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static void put(const char* fmt, ...) {
    if (s_copy_len + 1 >= sizeof(s_copy)) return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(s_copy + s_copy_len, sizeof(s_copy) - s_copy_len, fmt, args);
    va_end(args);
    if (n > 0) s_copy_len = s_copy_len + (u32)n < sizeof(s_copy) ? s_copy_len + (u32)n : (u32)sizeof(s_copy) - 1;
}

// Tab-separated table for spreadsheets: test, metric, unit, then each run's value.
static void copy_table() {
    s_copy_len = 0;
    s_copy[0] = 0;
    put("Test\tMetric\tUnit");
    for (u32 c = 0; c < g_cmp.run_count; ++c) put("\t%s", g_cmp.runs[c].label);
    put("\n");
    for (u32 i = 0; i < CI_COUNT; ++i) {
        put("System\t%s\t", g_cmp_info_names[i]);
        for (u32 c = 0; c < g_cmp.run_count; ++c) put("\t%s", g_cmp.runs[c].info[i]);
        put("\n");
    }
    static const char* const stat_names[] = { "", " min", " avg", " max" };
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        const CmpRow* r = &g_cmp.rows[i];
        const CmpTest* t = &g_cmp.tests[r->test];
        char x[32] = {};
        if (t->x_unit[0]) format_x(x, sizeof(x), r->x, t->x_unit);
        put("%s\t%s%s%s%s\t%s", t->title, r->name, stat_names[r->stat], x[0] ? " @ " : "", x, r->unit);
        for (u32 c = 0; c < g_cmp.run_count; ++c) {
            if (isnan(g_cmp.values[i][c])) put("\t");
            else put("\t%.6g", g_cmp.values[i][c]);
        }
        put("\n");
    }
    ImGui::SetClipboardText(s_copy);
    status("%s", "Table copied to clipboard (tab-separated)");
}

// ---- filters ------------------------------------------------------------------------------------

static u32 suite_list(const char* out[CMP_MAX_TESTS]) {
    u32 n = 0;
    for (u32 i = 0; i < g_cmp.test_count; ++i) {
        const char* s = g_cmp.tests[i].suite;
        bool seen = !s[0];
        for (u32 k = 0; k < n && !seen; ++k) seen = !strcmp(out[k], s);
        if (!seen) out[n++] = s;
    }
    return n;
}

static bool row_visible(const CmpRow* r, const char* suite) {
    const CmpTest* t = &g_cmp.tests[r->test];
    if (suite && strcmp(t->suite, suite) != 0) return false;
    return s_cu.filter.PassFilter(t->title) || s_cu.filter.PassFilter(r->name);
}

// ---- table --------------------------------------------------------------------------------------

static void text_right(const char* s, f32 right_x) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(right_x - ImGui::CalcTextSize(s).x);
    ImGui::TextUnformatted(s);
}

static void header_cell(u32 c) {
    const CmpRun* run = &g_cmp.runs[c];
    const bool ref = c == g_cmp.reference;
    ImGui::PushID((int)c);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const f32 w = ImGui::GetContentRegionAvail().x, line = ImGui::GetTextLineHeightWithSpacing();
    // File names wrap (mid-word if needed) onto at most two lines; date and reference marker below.
    ImGui::PushFont(g_fonts.medium, 0.0f);
    const f32 label_h = ImGui::CalcTextSize(run->label, nullptr, false, w).y;
    ImGui::PopFont();
    const f32 text_h = ImGui::GetTextLineHeight();
    const f32 label_text = label_h > text_h * 1.5f ? text_h * 2.0f : text_h;
    const f32 label_block = label_text + ImGui::GetStyle().ItemSpacing.y;
    ImGui::InvisibleButton("##hdr", ImVec2(w > 1.0f ? w : 1.0f, label_block + line * 2.0f));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) g_cmp.reference = c;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##run_menu");
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::PushFont(g_fonts.medium, 0.0f);
        ImGui::TextUnformatted(run->label);
        ImGui::PopFont();
        for (u32 i = 0; i < CI_COUNT; ++i) ImGui::Text("%s: %s", g_cmp_info_names[i], run->info[i]);
        ImGui::TextDisabled("Click: use as reference  \xC2\xB7  Right-click: more");
        ImGui::EndTooltip();
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::PushClipRect(start, ImVec2(start.x + w, start.y + label_text), true);
    ImGui::TextUnformatted(run->label);
    ImGui::PopClipRect();
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + label_block));
    ImGui::TextDisabled("%s", run->info[CI_DATE]);
    if (ref) ImGui::TextColored(g_pal.primary, "Reference");
    else ImGui::TextDisabled("%s", hovered ? "Use as reference" : "");

    bool removed = false;
    if (ImGui::BeginPopup("##run_menu")) {
        if (ImGui::MenuItem("Use as reference", nullptr, ref)) g_cmp.reference = c;
        if (ImGui::MenuItem("Remove")) removed = true;
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (removed) compare_remove(c);     // after PopID; the table skips the rest of this frame's stale cells
}

static void value_cell(u32 row, u32 c, f32 delta_w) {
    const f64 v = g_cmp.values[row][c], base = g_cmp.values[row][g_cmp.reference];
    const f32 x0 = ImGui::GetCursorPosX(), w = ImGui::GetContentRegionAvail().x;
    const bool delta = s_cu.show_change && c != g_cmp.reference && g_cmp.run_count > 1;
    const f32 value_right = x0 + w - (s_cu.show_change && g_cmp.run_count > 1 ? delta_w : 0.0f);
    ImGui::TextUnformatted("");
    if (isnan(v)) {
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.outline);
        text_right("\xE2\x80\x94", value_right);    // em dash
        ImGui::PopStyleColor();
        return;
    }
    char s[32];
    format_value(s, sizeof(s), v);
    theme_push_numbers();
    text_right(s, value_right);
    if (delta && !isnan(base)) {
        const f64 pct = delta_pct(v, base);
        const i32 rating = delta_rating(pct, g_cmp.rows[row].unit);
        format_delta(s, sizeof(s), pct);
        ImGui::PushStyleColor(ImGuiCol_Text, rating > 0 ? g_pal.ok : rating < 0 ? g_pal.error : g_pal.on_surface_variant);
        text_right(s, x0 + w);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
}

static void group_row(const char* title, u32 cols) {
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(g_pal.container));
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    (void)cols;
}

static void draw_table() {
    const u32 runs = g_cmp.run_count, cols = 2 + runs;
    const f32 fs = ImGui::GetFontSize();
    theme_push_numbers();
    const f32 delta_w = ImGui::CalcTextSize("+100.0%").x + fs * 0.8f;
    const f32 value_w = ImGui::CalcTextSize("12345.6").x;
    ImGui::PopFont();
    const f32 run_w = value_w + delta_w + fs * 1.5f;     // same width with or without the change shown

    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("cmp", (int)cols, flags)) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, fs * 16.0f);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, fs * 4.5f);
    for (u32 c = 0; c < runs; ++c) {
        ImGui::PushID((int)c);
        ImGui::TableSetupColumn("##run", ImGuiTableColumnFlags_WidthFixed, run_w);
        ImGui::PopID();
    }

    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted("Metric");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted("Unit");
    ImGui::PopFont();
    ImGui::PopStyleColor();
    for (u32 c = 0; c < runs && c < g_cmp.run_count; ++c) {
        ImGui::TableSetColumnIndex((int)(2 + c));
        header_cell(c);
    }
    if (g_cmp.run_count != runs) { ImGui::EndTable(); return; }     // a run was removed: redraw next frame

    if (s_cu.show_system) {
        group_row("System", cols);
        const CmpRun* ref = &g_cmp.runs[g_cmp.reference];
        for (u32 i = 0; i < CI_COUNT; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent();
            ImGui::TextUnformatted(g_cmp_info_names[i]);
            ImGui::Unindent();
            for (u32 c = 0; c < runs; ++c) {
                ImGui::TableSetColumnIndex((int)(2 + c));
                const char* s = g_cmp.runs[c].info[i];
                const bool differs = c != g_cmp.reference && strcmp(s, ref->info[i]) != 0 && i != CI_DATE;
                ImGui::PushStyleColor(ImGuiCol_Text, differs ? g_pal.warn : g_pal.on_surface);
                ImGui::TextUnformatted(s[0] ? s : "\xE2\x80\x94");
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && s[0]) ImGui::SetTooltip("%s", s);
            }
        }
    }

    const char* suites[CMP_MAX_TESTS];
    const u32 suite_count = suite_list(suites);
    const char* suite = s_cu.suite && s_cu.suite <= suite_count ? suites[s_cu.suite - 1] : nullptr;
    static const char* const stat_names[] = { "", "min", "avg", "max" };
    i32 last_test = -1;
    char label[128], x[32];
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        const CmpRow* r = &g_cmp.rows[i];
        if (!row_visible(r, suite)) continue;
        const CmpTest* t = &g_cmp.tests[r->test];
        if ((i32)r->test != last_test) {
            group_row(t->title, cols);
            last_test = r->test;
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (r->stat != CS_VALUE) {
            snprintf(label, sizeof(label), "%s %s", r->name, stat_names[r->stat]);
        } else if (t->x_unit[0]) {
            format_x(x, sizeof(x), r->x, t->x_unit);
            snprintf(label, sizeof(label), "%s @ %s", r->name, x);
        } else {
            snprintf(label, sizeof(label), "%s", r->name);
        }
        ImGui::Indent();
        ImGui::TextUnformatted(label);
        ImGui::Unindent();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", r->unit);
        for (u32 c = 0; c < runs; ++c) {
            ImGui::TableSetColumnIndex((int)(2 + c));
            value_cell(i, c, delta_w);
        }
    }
    ImGui::EndTable();
}

// ---- frame --------------------------------------------------------------------------------------

void compare_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##compare", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::BeginDisabled(runner_is_running());      // the file dialog would load shell extensions mid-benchmark
    if (theme_button_primary("Add files...")) add_files_dialog();
    ImGui::SameLine();
    if (theme_button("Add current run")) add_current();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_cmp.run_count == 0);
    if (theme_button("Copy table")) copy_table();
    ImGui::SameLine();
    if (theme_button("Clear")) { compare_clear(); s_cu.status[0] = 0; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%u / %u runs%s%s", g_cmp.run_count, CMP_MAX_RUNS, s_cu.status[0] ? "  |  " : "", s_cu.status);

    if (g_cmp.run_count == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Add saved results (*.toon.txt) with \"Add files...\" or by dropping them on this window,");
        ImGui::TextDisabled("or add the results of this session with \"Add current run\".");
        ImGui::End();
        return;
    }

    // Filters: suite tabs, search, toggles.
    const char* suites[CMP_MAX_TESTS];
    const u32 suite_count = suite_list(suites);
    if (s_cu.suite > suite_count) s_cu.suite = 0;
    if (theme_tab("All", s_cu.suite == 0)) s_cu.suite = 0;
    for (u32 i = 0; i < suite_count; ++i) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PushID((int)i);
        if (theme_tab(suites[i], s_cu.suite == i + 1)) s_cu.suite = i + 1;
        ImGui::PopID();
    }
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::InputTextWithHint("##search", "Search tests / metrics", s_cu.filter.InputBuf, IM_ARRAYSIZE(s_cu.filter.InputBuf)))
        s_cu.filter.Build();
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    theme_checkbox("Show change", &s_cu.show_change);
    ImGui::SameLine();
    theme_checkbox("System info", &s_cu.show_system);

    ImGui::Spacing();
    theme_begin_panel("cmp_panel", ImVec2(0, 0), false);
    draw_table();
    theme_end_panel();
    ImGui::End();
}
