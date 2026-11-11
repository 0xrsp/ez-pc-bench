// Compare window: toolbar, filters and one wide table. Columns: metric, unit, then one per run; each run
// cell shows the raw value and its change vs the reference run (green = better, red = worse, > 1%).
#include "app/compare_ui.h"
#include "app/library.h"
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
    bool show_summary = true;
    bool library_request;       // "Library..." clicked: open the popup this frame
    bool lib_selected[256];
    i32 set_open = -1;          // "Expand all" (1) / "Collapse all" (0) for the next table frame
    ImGuiTextFilter lib_filter;
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
    ofn.lpstrFilter = L"EzPcBench results (*.toon.txt)\0*.toon.txt\0All files\0*.*\0";
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
    char s[32], fps[32];
    format_value(s, sizeof(s), v);
    const CmpRow* r = &g_cmp.rows[row];
    const bool has_fps = format_frame_fps(fps, sizeof(fps), r->name, r->unit, v);
    theme_push_numbers();
    // Value right-aligned before the change column; frame times get the equivalent fps (dimmed) after it.
    const f32 fps_w = has_fps ? ImGui::CalcTextSize(fps).x : 0.0f;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(value_right - ImGui::CalcTextSize(s).x - fps_w);
    ImGui::TextUnformatted(s);
    if (has_fps) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s", fps);
    }
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

// Section header row (Summary, System, one per test): click anywhere on it to collapse / expand its rows.
// Returns whether it is open. "Expand all" / "Collapse all" apply through s_cu.set_open for one frame.
static bool group_row(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(g_pal.container));
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    if (s_cu.set_open >= 0) ImGui::SetNextItemOpen(s_cu.set_open != 0);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(g_pal.on_surface.x, g_pal.on_surface.y, g_pal.on_surface.z, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(g_pal.on_surface.x, g_pal.on_surface.y, g_pal.on_surface.z, 0.10f));
    const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns |
                                                   ImGuiTreeNodeFlags_NoTreePushOnOpen);
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
    return open;
}

// "Single thread", "Read @ 16 KiB", "One-way latency avg".
static void row_label(const CmpRow* r, char* buf, u32 size) {
    static const char* const stat_names[] = { "", "min", "avg", "max" };
    const CmpTest* t = &g_cmp.tests[r->test];
    if (r->stat != CS_VALUE) {
        snprintf(buf, size, "%s %s", r->name, stat_names[r->stat]);
    } else if (t->x_unit[0]) {
        char x[32];
        format_x(x, sizeof(x), r->x, t->x_unit);
        snprintf(buf, size, "%s @ %s", r->name, x);
    } else {
        snprintf(buf, size, "%s", r->name);
    }
}

// ---- summary ------------------------------------------------------------------------------------

static void summary_label(const char* label, const char* tooltip, bool emphasis) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Indent();
    if (emphasis) ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(label);
    if (emphasis) ImGui::PopFont();
    ImGui::Unindent();
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
}

static void pct_cell(f64 pct, bool big) {
    char s[32];
    format_delta(s, sizeof(s), pct);
    if (big) ImGui::PushFont(g_fonts.medium, theme_font_size(1.2f));
    else theme_push_numbers();
    ImGui::TextColored(pct >= 1.0 ? g_pal.ok : pct <= -1.0 ? g_pal.error : g_pal.on_surface_variant, "%s", s);
    ImGui::PopFont();
}

static void dash_cell() { ImGui::TextColored(g_pal.outline, "\xE2\x80\x94"); }

// Which metric a largest gain / loss came from.
static void row_tooltip(u32 row) {
    if (!ImGui::IsItemHovered()) return;
    const CmpRow* r = &g_cmp.rows[row];
    char label[128];
    row_label(r, label, sizeof(label));
    ImGui::SetTooltip("%s: %s (%s)", g_cmp.tests[r->test].title, label, r->unit);
}

static void draw_summary(u32 runs, const char* const* suites, u32 suite_count) {
    const bool open = group_row("Summary vs reference");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Performance change against the reference run: + = faster / better, also for latency and time.");
    if (!open) return;
    CmpSummary sum[CMP_MAX_RUNS];
    bool have[CMP_MAX_RUNS];
    for (u32 c = 0; c < runs; ++c) have[c] = c != g_cmp.reference && compare_summary(c, nullptr, &sum[c]);

    summary_label("Overall", "Geometric mean of the per-metric ratios over all metrics both runs have.\n"
                  "Latency and time are flipped so + always means faster. Left out: ratios (x, %), constants and\n"
                  "p99.9 frame times (a handful of frames, too noisy to average).", true);
    for (u32 c = 0; c < runs; ++c) {
        ImGui::TableSetColumnIndex((int)(2 + c));
        if (c == g_cmp.reference) ImGui::TextDisabled("reference");
        else if (have[c]) pct_cell(sum[c].geomean_pct, true);
        else dash_cell();
    }
    for (u32 k = 0; suite_count > 1 && k < suite_count; ++k) {
        summary_label(suites[k], nullptr, false);
        for (u32 c = 0; c < runs; ++c) {
            ImGui::TableSetColumnIndex((int)(2 + c));
            CmpSummary s;
            if (c == g_cmp.reference) continue;
            if (compare_summary(c, suites[k], &s)) pct_cell(s.geomean_pct, false);
            else dash_cell();
        }
    }
    summary_label("Faster / slower / same", "Metrics more than 1% faster / slower than the reference, and within 1%.", false);
    for (u32 c = 0; c < runs; ++c) {
        ImGui::TableSetColumnIndex((int)(2 + c));
        if (!have[c]) continue;
        theme_push_numbers();
        ImGui::TextColored(g_pal.ok, "%u", sum[c].better);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled(" / ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(g_pal.error, "%u", sum[c].worse);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled(" / %u", sum[c].same);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%u metrics compared", sum[c].compared);
    }
    summary_label("Largest gain", "Hover a value to see the metric.", false);
    for (u32 c = 0; c < runs; ++c) {
        ImGui::TableSetColumnIndex((int)(2 + c));
        if (!have[c]) continue;
        pct_cell(sum[c].best_pct, false);
        row_tooltip(sum[c].best_row);
    }
    summary_label("Largest loss", "Hover a value to see the metric.", false);
    for (u32 c = 0; c < runs; ++c) {
        ImGui::TableSetColumnIndex((int)(2 + c));
        if (!have[c]) continue;
        pct_cell(sum[c].worst_pct, false);
        row_tooltip(sum[c].worst_row);
    }
}

static void draw_table() {
    const u32 runs = g_cmp.run_count, cols = 2 + runs;
    const f32 fs = ImGui::GetFontSize();
    theme_push_numbers();
    const f32 delta_w = ImGui::CalcTextSize("+100.0%").x + fs * 0.8f;
    f32 value_w = ImGui::CalcTextSize("12345.6").x;
    char fps[32];
    for (u32 i = 0; i < g_cmp.row_count; ++i)     // room for " (1234 fps)" after frame times
        if (format_frame_fps(fps, sizeof(fps), g_cmp.rows[i].name, g_cmp.rows[i].unit, 1.0)) {
            value_w = ImGui::CalcTextSize("0.123 (12345 fps)").x;
            break;
        }
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

    const char* suites[CMP_MAX_TESTS];
    const u32 suite_count = suite_list(suites);
    if (s_cu.show_summary && runs > 1) draw_summary(runs, suites, suite_count);

    if (s_cu.show_system && group_row("System")) {
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

    const char* suite = s_cu.suite && s_cu.suite <= suite_count ? suites[s_cu.suite - 1] : nullptr;
    i32 last_test = -1;
    bool test_open = true;
    char label[128];
    for (u32 i = 0; i < g_cmp.row_count; ++i) {
        const CmpRow* r = &g_cmp.rows[i];
        if (!row_visible(r, suite)) continue;
        const CmpTest* t = &g_cmp.tests[r->test];
        if ((i32)r->test != last_test) {
            test_open = group_row(t->title);
            last_test = r->test;
        }
        if (!test_open) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        row_label(r, label, sizeof(label));
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
    s_cu.set_open = -1;
}

// ---- library ------------------------------------------------------------------------------------

// Built-in reference runs (library/*.toon.txt, compressed into the exe): pick some and add them as columns.
static void draw_library() {
    if (s_cu.library_request) {
        s_cu.library_request = false;
        memset(s_cu.lib_selected, 0, sizeof(s_cu.lib_selected));
        ImGui::OpenPopup("Library");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.85f, vp->WorkSize.y * 0.75f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Library", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    const u32 n = library_load();
    ImGui::TextDisabled("Reference runs by the developer, built into the app. Tick the ones to compare.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::InputTextWithHint("##lib_search", "Search CPU / GPU / name", s_cu.lib_filter.InputBuf,
                                 IM_ARRAYSIZE(s_cu.lib_filter.InputBuf)))
        s_cu.lib_filter.Build();

    const f32 buttons_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    u32 picked = 0;
    bool lib_add_now = false;
    if (ImGui::BeginTable("lib", 7, flags, ImVec2(0.0f, -buttons_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme_checkbox_height());
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("RAM", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Power", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        for (u32 i = 0; i < n && i < ARRAY_COUNT(s_cu.lib_selected); ++i) {
            const LibraryRun* r = library_run(i);
            if (!s_cu.lib_filter.PassFilter(r->name) && !s_cu.lib_filter.PassFilter(r->info.info[CI_CPU]) &&
                !s_cu.lib_filter.PassFilter(r->info.info[CI_GPU]))
                continue;
            const bool added = compare_has_label(r->name);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            bool on = added || s_cu.lib_selected[i];
            ImGui::BeginDisabled(added);
            // Rows are a frame tall (the name column aligns to frame padding); center the smaller box on that.
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - theme_checkbox_height()) * 0.5f);
            if (theme_checkbox("##pick", &on) && !added) s_cu.lib_selected[i] = on;
            ImGui::EndDisabled();
            if (added && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Already in the comparison");
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            // The whole row is clickable: click toggles, double-click adds this run right away.
            ImGui::BeginDisabled(added);
            if (ImGui::Selectable(r->name, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    memset(s_cu.lib_selected, 0, sizeof(s_cu.lib_selected));
                    s_cu.lib_selected[i] = true;
                    lib_add_now = true;
                } else {
                    s_cu.lib_selected[i] = !s_cu.lib_selected[i];
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r->info.info[CI_CPU]);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r->info.info[CI_GPU]);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r->info.info[CI_RAM]);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r->info.info[CI_POWER]);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r->info.info[CI_DATE]);
        }
        ImGui::EndTable();
    }
    for (u32 i = 0; i < n && i < ARRAY_COUNT(s_cu.lib_selected); ++i) picked += s_cu.lib_selected[i];
    if (n == 0) ImGui::TextDisabled("This build has no built-in runs.");

    char add_label[48];
    snprintf(add_label, sizeof(add_label), picked ? "Add %u run%s" : "Add", picked, picked == 1 ? "" : "s");
    ImGui::BeginDisabled(picked == 0);
    const bool add = theme_button_primary(add_label) || (lib_add_now && picked);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme_button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    if (add) {
        u32 ok = 0;
        char err[200] = {};
        for (u32 i = 0; i < n && i < ARRAY_COUNT(s_cu.lib_selected); ++i) {
            if (!s_cu.lib_selected[i]) continue;
            const LibraryRun* r = library_run(i);
            if (compare_add_buffer(r->name, r->data, r->size, err, sizeof(err))) ++ok;
        }
        if (ok == picked) snprintf(s_cu.status, sizeof(s_cu.status), "Added %u run%s from the library", ok, ok == 1 ? "" : "s");
        else snprintf(s_cu.status, sizeof(s_cu.status), "Added %u of %u runs: %s", ok, picked, err);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
    if (theme_button("Library...")) s_cu.library_request = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(g_cmp.run_count == 0);
    if (theme_button("Copy table")) copy_table();
    ImGui::SameLine();
    if (theme_button("Clear")) { compare_clear(); s_cu.status[0] = 0; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%u / %u runs%s%s", g_cmp.run_count, CMP_MAX_RUNS, s_cu.status[0] ? "  |  " : "", s_cu.status);
    draw_library();

    if (g_cmp.run_count == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Add saved results (*.toon.txt) with \"Add files...\" or by dropping them on this window,");
        ImGui::TextDisabled("add the results of this session with \"Add current run\", or pick reference runs from \"Library...\".");
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
    theme_checkbox("Summary", &s_cu.show_summary);
    ImGui::SameLine();
    theme_checkbox("System info", &s_cu.show_system);
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 1.5f);
    if (theme_tab("Expand all", false)) s_cu.set_open = 1;
    ImGui::SameLine(0.0f, 4.0f);
    if (theme_tab("Collapse all", false)) s_cu.set_open = 0;

    ImGui::Spacing();
    theme_begin_panel("cmp_panel", ImVec2(0, 0), false);
    draw_table();
    theme_end_panel();
    ImGui::End();
}
