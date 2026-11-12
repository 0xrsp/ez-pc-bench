// History window: results\history\*.toon.txt, trend chart per metric / score, runs list (see history_ui.h).
#include "app/history_ui.h"
#include "app/score_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/baseline.h"
#include "core/compare.h"
#include "core/history.h"
#include "core/results.h"
#include "core/runfile.h"
#include "core/runner.h"
#include "core/score.h"
#include "core/sysinfo.h"
#include "core/toon.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr u32 HIST_MAX_RUNS = 512;
constexpr u32 HIST_MAX_KEYS = 4096;
constexpr u32 HIST_HASH = 8192;
constexpr u32 HIST_MAX_VALUES = HIST_MAX_RUNS * 1024;

struct HistKey {
    char test[48], name[64], unit[16];
    f64 x;
};

struct HistRun {
    wchar_t file[MAX_PATH];
    char name[96];          // file name without .toon.txt
    char stamp[24];         // "2026-11-11 15:07"
    char cpu[48], gpu[48];  // short names
    f64 overall, comp[SC_COUNT];
    u32 flags;
    u32 first, count;       // slice of HistData::values
    bool picked;
};

struct HistVal {
    u32 key;
    f64 value;
};

struct HistData {
    HistRun runs[HIST_MAX_RUNS];
    HistKey keys[HIST_MAX_KEYS];
    u32 hash[HIST_HASH];    // key index + 1, 0 = empty
    HistVal values[HIST_MAX_VALUES];
};

struct HistoryUi {
    HistData* d;            // VirtualAlloc'd while the window is open
    u32 run_count, key_count, value_count, skipped;
    i32 sel;                // < SCORE_ITEMS: score item (score_item), else key index + that
    char status[300];
    bool about;
};
static HistoryUi s_hu;

constexpr i32 SCORE_ITEMS = SCORE_ITEM_COUNT;

static u32 key_hash(const char* test, const char* name, f64 x) {
    u32 h = 2166136261u;
    for (const char* p = test; *p; ++p) h = (h ^ (u8)*p) * 16777619u;
    h = (h ^ 0x1f) * 16777619u;
    for (const char* p = name; *p; ++p) h = (h ^ (u8)*p) * 16777619u;
    const u64 xb = (u64)x;
    for (u32 i = 0; i < 8; ++i) h = (h ^ (u8)(xb >> (i * 8))) * 16777619u;
    return h;
}

static u32 intern_key(const char* test, const char* name, const char* unit, f64 x) {
    HistData* d = s_hu.d;
    for (u32 h = key_hash(test, name, x) % HIST_HASH, probe = 0; probe < HIST_HASH; h = (h + 1) % HIST_HASH, ++probe) {
        const u32 slot = d->hash[h];
        if (!slot) {
            if (s_hu.key_count >= HIST_MAX_KEYS) return UINT32_MAX;
            HistKey* k = &d->keys[s_hu.key_count];
            snprintf(k->test, sizeof(k->test), "%s", test);
            snprintf(k->name, sizeof(k->name), "%s", name);
            snprintf(k->unit, sizeof(k->unit), "%s", unit);
            k->x = x;
            d->hash[h] = ++s_hu.key_count;
            return s_hu.key_count - 1;
        }
        const HistKey* k = &d->keys[slot - 1];
        if (k->x == x && !strcmp(k->test, test) && !strcmp(k->name, name)) return slot - 1;
    }
    return UINT32_MAX;
}

static f64 run_value(const HistRun* r, i32 sel);

static bool any_value(i32 sel) {
    for (u32 i = 0; i < s_hu.run_count; ++i)
        if (isfinite(run_value(&s_hu.d->runs[i], sel))) return true;
    return false;
}

static int cmp_runs(const void* a, const void* b) {
    return strcmp(((const HistRun*)a)->stamp, ((const HistRun*)b)->stamp);
}

static void load() {
    s_hu.run_count = s_hu.key_count = s_hu.value_count = s_hu.skipped = 0;
    memset(s_hu.d->hash, 0, sizeof(s_hu.d->hash));
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    history_dir(dir);
    swprintf(pattern, MAX_PATH, L"%s\\*.toon.txt", dir);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    RunFile* rf = rf_scratch();
    do {
        if (s_hu.run_count >= HIST_MAX_RUNS) break;
        HistRun* h = &s_hu.d->runs[s_hu.run_count];
        memset(h, 0, sizeof(*h));
        swprintf(h->file, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        ToonReader r;
        char err[64];
        char* buf = toon_read_file(h->file, &r, err, sizeof(err));
        if (!buf) { s_hu.skipped++; continue; }
        const bool ok = rf_parse(r.p, (u32)(r.end - r.p), rf, err, sizeof(err));
        toon_free_file(buf);
        // Only benchmark runs: other kinds (stability reports) have their own tools.
        if (!ok || (rf->kind[0] && strcmp(rf->kind, "benchmark") != 0) || !rf->metric_count) { s_hu.skipped++; continue; }
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, h->name, sizeof(h->name), nullptr, nullptr);
        if (char* ext = strstr(h->name, ".toon.txt")) *ext = 0;
        snprintf(h->stamp, sizeof(h->stamp), "%.16s", rf->timestamp);
        if (char* t = strchr(h->stamp, 'T')) *t = ' ';
        cpu_short_name(rf->cpu, h->cpu, sizeof(h->cpu));
        gpu_short_name(rf->gpu, h->gpu, sizeof(h->gpu));
        ScoreResult sr;
        score_compute(rf, &sr);
        h->overall = sr.overall;
        memcpy(h->comp, sr.comp, sizeof(h->comp));
        h->flags = sr.any ? sr.flags : 0;
        h->first = s_hu.value_count;
        for (u32 i = 0; i < rf->metric_count && s_hu.value_count < HIST_MAX_VALUES; ++i) {
            const RfMetric* m = &rf->metrics[i];
            const u32 k = intern_key(rf->tests[m->test].id, m->name, m->unit, m->x);
            if (k == UINT32_MAX) continue;
            s_hu.d->values[s_hu.value_count++] = { k, m->value };
        }
        h->count = s_hu.value_count - h->first;
        s_hu.run_count++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    qsort(s_hu.d->runs, s_hu.run_count, sizeof(HistRun), cmp_runs);
    if (s_hu.sel >= SCORE_ITEMS + (i32)s_hu.key_count) s_hu.sel = 0;
    // Start on a score item some run has (partial runs have components but no Overall).
    if (s_hu.sel < SCORE_ITEMS && !any_value(s_hu.sel))
        for (i32 item = 0; item < SCORE_ITEMS; ++item)
            if (any_value(item)) { s_hu.sel = item; break; }
    snprintf(s_hu.status, sizeof(s_hu.status), "%u run%s loaded%s", s_hu.run_count, s_hu.run_count == 1 ? "" : "s",
             s_hu.skipped ? " (other files skipped)" : "");
}

void history_ui_on_open() {
    if (!s_hu.d) s_hu.d = (HistData*)VirtualAlloc(nullptr, sizeof(HistData), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (s_hu.d) load();
}

void history_ui_on_close() {
    if (s_hu.d) VirtualFree(s_hu.d, 0, MEM_RELEASE);
    s_hu.d = nullptr;
    s_hu.run_count = s_hu.key_count = s_hu.value_count = 0;
}

// Value of the selected item in run r (NaN if missing).
static f64 run_value(const HistRun* r, i32 sel) {
    if (sel == 0) return r->overall;
    if (sel < SCORE_ITEMS) return r->comp[sel - 1];
    const u32 key = (u32)(sel - SCORE_ITEMS);
    for (u32 i = 0; i < r->count; ++i)
        if (s_hu.d->values[r->first + i].key == key) return s_hu.d->values[r->first + i].value;
    return NAN;
}

static void item_label(i32 sel, char* buf, u32 size) {
    if (sel < SCORE_ITEMS) { snprintf(buf, size, "Score: %s", score_item_name((u32)sel)); return; }
    const HistKey* k = &s_hu.d->keys[sel - SCORE_ITEMS];
    if (k->x != 0.0) snprintf(buf, size, "%s  \xC2\xB7  %s @ %g (%s)", k->test, k->name, k->x, k->unit);
    else snprintf(buf, size, "%s  \xC2\xB7  %s (%s)", k->test, k->name, k->unit);
}

static const char* item_unit(i32 sel) { return sel < SCORE_ITEMS ? "" : s_hu.d->keys[sel - SCORE_ITEMS].unit; }

static void format_item(char* buf, u32 size, i32 sel, f64 v) {
    if (!isfinite(v)) snprintf(buf, size, "\xE2\x80\x94");
    else if (sel < SCORE_ITEMS) snprintf(buf, size, "%.0f", v);
    else format_value(buf, size, v);
}

static void draw_picker() {
    char label[200];
    item_label(s_hu.sel, label, sizeof(label));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Trend of");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
    if (ImGui::BeginCombo("##item", label, ImGuiComboFlags_HeightLarge)) {
        static ImGuiTextFilter filter;
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); filter.Clear(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##filter", "Search tests / metrics", filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf)))
            filter.Build();
        const i32 n = SCORE_ITEMS + (i32)s_hu.key_count;
        ImGuiListClipper clip;      // thousands of keys with series points: draw only the visible ones
        static i32 shown[SCORE_ITEMS + HIST_MAX_KEYS];
        i32 count = 0;
        for (i32 i = 0; i < n; ++i) {
            item_label(i, label, sizeof(label));
            if (filter.PassFilter(label)) shown[count++] = i;
        }
        clip.Begin(count);
        while (clip.Step())
            for (i32 j = clip.DisplayStart; j < clip.DisplayEnd; ++j) {
                item_label(shown[j], label, sizeof(label));
                ImGui::PushID(shown[j]);
                if (ImGui::Selectable(label, shown[j] == s_hu.sel)) s_hu.sel = shown[j];
                ImGui::PopID();
            }
        ImGui::EndCombo();
    }
}

static void draw_chart(f32 height) {
    const u32 n = s_hu.run_count;
    static f64 vals[HIST_MAX_RUNS];
    f64 lo = INFINITY, hi = -INFINITY;
    u32 points = 0;
    for (u32 i = 0; i < n; ++i) {
        vals[i] = run_value(&s_hu.d->runs[i], s_hu.sel);
        if (!isfinite(vals[i])) continue;
        lo = vals[i] < lo ? vals[i] : lo;
        hi = vals[i] > hi ? vals[i] : hi;
        ++points;
    }
    const f32 fs = ImGui::GetFontSize();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const f32 w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##chart", ImVec2(w, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + height), ImGui::GetColorU32(g_pal.container), fs * 0.4f);
    if (!points) {
        dl->AddText(ImVec2(p0.x + fs, p0.y + fs), ImGui::GetColorU32(g_pal.on_surface_variant), "No run has this value.");
        return;
    }
    const f64 pad = hi > lo ? (hi - lo) * 0.15 : (hi != 0.0 ? fabs(hi) * 0.05 : 1.0);
    lo -= pad;
    hi += pad;
    const f32 left = p0.x + fs * 4.5f, right = p0.x + w - fs, top = p0.y + fs * 0.8f, bottom = p0.y + height - fs * 1.6f;
    char t[32];
    const ImU32 dim = ImGui::GetColorU32(g_pal.on_surface_variant), grid = ImGui::GetColorU32(g_pal.outline_variant);
    for (u32 g = 0; g <= 4; ++g) {      // horizontal grid with values
        const f32 y = bottom - (bottom - top) * g / 4.0f;
        dl->AddLine(ImVec2(left, y), ImVec2(right, y), grid, 1.0f);
        format_item(t, sizeof(t), s_hu.sel, lo + (hi - lo) * g / 4.0);
        dl->AddText(ImVec2(p0.x + fs * 0.4f, y - fs * 0.55f), dim, t);
    }
    auto px = [&](u32 i) { return n > 1 ? left + (right - left) * i / (f32)(n - 1) : (left + right) * 0.5f; };
    auto py = [&](f64 v) { return bottom - (f32)((v - lo) / (hi - lo)) * (bottom - top); };
    const ImU32 line = ImGui::GetColorU32(g_pal.primary);
    i32 prev = -1;
    for (u32 i = 0; i < n; ++i) {
        if (!isfinite(vals[i])) continue;
        if (prev >= 0) dl->AddLine(ImVec2(px(prev), py(vals[prev])), ImVec2(px(i), py(vals[i])), line, 2.0f);
        prev = (i32)i;
    }
    i32 near_i = -1;
    f32 near_d = fs * 1.2f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (u32 i = 0; i < n; ++i) {
        if (!isfinite(vals[i])) continue;
        const HistRun* r = &s_hu.d->runs[i];
        const bool flagged = (r->flags & ~(SF_INCOMPLETE | SF_NO_CONDITIONS)) != 0;
        dl->AddCircleFilled(ImVec2(px(i), py(vals[i])), fs * 0.28f, ImGui::GetColorU32(flagged ? g_pal.warn : g_pal.primary));
        const f32 d = fabsf(mouse.x - px(i));
        if (hovered && d < near_d) { near_d = d; near_i = (i32)i; }
    }
    // First / last date under the axis.
    dl->AddText(ImVec2(left, bottom + fs * 0.3f), dim, s_hu.d->runs[0].stamp);
    if (n > 1) {
        const char* last = s_hu.d->runs[n - 1].stamp;
        dl->AddText(ImVec2(right - ImGui::CalcTextSize(last).x, bottom + fs * 0.3f), dim, last);
    }
    if (near_i >= 0) {
        const HistRun* r = &s_hu.d->runs[near_i];
        dl->AddCircle(ImVec2(px(near_i), py(vals[near_i])), fs * 0.5f, line, 16, 2.0f);
        format_item(t, sizeof(t), s_hu.sel, vals[near_i]);
        char flags[300];
        score_flag_text(r->flags & ~SF_INCOMPLETE, flags, sizeof(flags));
        ImGui::SetTooltip("%s  %s %s\n%s\n%s, %s%s%s", r->stamp, t, item_unit(s_hu.sel), r->name, r->cpu, r->gpu,
                          flags[0] ? "\nFlags: " : "", flags);
    }
}

static void compare_picked() {
    u32 added = 0;
    char err[200] = {};
    for (u32 i = 0; i < s_hu.run_count; ++i)
        if (s_hu.d->runs[i].picked && compare_add_file(s_hu.d->runs[i].file, err, sizeof(err))) ++added;
    snprintf(s_hu.status, sizeof(s_hu.status), "Added %u run%s to the compare window%s%s", added, added == 1 ? "" : "s",
             err[0] ? ": " : "", err);
    if (added) ui_request_tool(TOOL_COMPARE);
}

void history_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##history", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    score_ui_about(&s_hu.about);
    if (!s_hu.d) {
        ImGui::TextDisabled("Out of memory.");
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(runner_is_running());
    if (theme_button_primary("Refresh")) load();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme_button("Open folder")) {
        wchar_t dir[MAX_PATH];
        history_dir(dir);
        ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    u32 picked = 0;
    for (u32 i = 0; i < s_hu.run_count; ++i) picked += s_hu.d->runs[i].picked;
    ImGui::BeginDisabled(picked == 0 || runner_is_running());
    char cmp[48];
    snprintf(cmp, sizeof(cmp), picked ? "Compare %u" : "Compare", picked);
    if (theme_button(cmp)) compare_picked();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme_button("About the score")) s_hu.about = true;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", s_hu.status);

    if (!s_hu.run_count) {
        ImGui::Spacing();
        ImGui::TextDisabled("No runs yet. Every finished run is saved to results\\history next to the exe.");
        ImGui::End();
        return;
    }
    theme_group_begin("Trend");
    draw_picker();
    ImGui::Spacing();
    draw_chart(ImGui::GetFontSize() * 16.0f);
    theme_group_end();

    // Runs, newest first.
    theme_begin_panel("hist_runs", ImVec2(0, 0), false);
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("runs", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, theme_checkbox_height());
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("CPU / GPU", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Overall", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Selected", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        char t[32], flags_text[300];
        for (i32 i = (i32)s_hu.run_count - 1; i >= 0; --i) {
            HistRun* r = &s_hu.d->runs[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            theme_checkbox("##pick", &r->picked);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r->stamp);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r->name);
            ImGui::TableNextColumn();
            ImGui::Text("%s, %s", r->cpu, r->gpu);
            ImGui::TableNextColumn();
            format_item(t, sizeof(t), 0, r->overall);
            theme_push_numbers();
            ImGui::TextUnformatted(t);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            format_item(t, sizeof(t), s_hu.sel, run_value(r, s_hu.sel));
            theme_push_numbers();
            ImGui::TextUnformatted(t);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            score_flag_text(r->flags, flags_text, sizeof(flags_text));
            ImGui::TextColored(g_pal.warn, "%s", flags_text);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    theme_end_panel();
    ImGui::End();
}
