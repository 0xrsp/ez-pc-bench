// Score tab, ranking tab and the "About the score" page (see score_ui.h).
#include "app/score_ui.h"
#include "app/graph.h"
#include "app/library.h"
#include "app/snapshot.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/baseline.h"
#include "core/presets.h"
#include "core/results.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "imgui.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- shared pieces ------------------------------------------------------------------------------------

static void score_text(char* buf, u32 size, f64 v) {
    if (isfinite(v)) snprintf(buf, size, "%.0f", v);
    else snprintf(buf, size, "\xE2\x80\x94");      // em dash
}

// Horizontal bar of `w` x `h` at the cursor: filled to `frac`, with a thin marker at `marker` (the reference).
static void bar(f32 frac, f32 marker, f32 w, f32 h, const ImVec4& col) {
    graph_bar(frac, marker, ImVec2(w, h), ImGui::GetColorU32(col));
}

void score_ui_inputs_table(const ScoreResult* s, ScoreComp c) {
    if (!ImGui::BeginTable("##inputs", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupColumn("Input");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Reference");
    ImGui::TableSetupColumn("vs ref");
    ImGui::TableSetupColumn("Weight");
    ImGui::TableSetupColumn("Test");
    ImGui::TableHeadersRow();
    char v[32], r[32];
    for (u32 i = 0; i < g_score.input_count; ++i) {
        const ScoreInput* in = &g_score.inputs[i];
        if (in->comp != c) continue;
        const ScoreInputValue* iv = &s->in[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(in->label);
        ImGui::TableNextColumn();
        format_value(r, sizeof(r), in->ref);
        if (iv->present) {
            format_value(v, sizeof(v), iv->value);
            ImGui::Text("%s %s", v, in->unit);
        } else {
            ImGui::TextColored(g_pal.warn, "missing");
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s %s", r, in->unit);
        ImGui::TableNextColumn();
        if (iv->present) {
            const f64 pct = (iv->ratio - 1.0) * 100.0;
            ImGui::TextColored(pct >= 1.0 ? g_pal.ok : pct <= -1.0 ? g_pal.error : g_pal.on_surface_variant, "%+.1f%%", pct);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", in->weight);
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s%s", iv->present ? iv->test : in->test, in->lower_better ? "  (lower is better)" : "");
    }
    ImGui::EndTable();
}

void score_ui_tooltip(const ScoreResult* s, const char* title) {
    ImGui::BeginTooltip();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::Text("%s  \xC2\xB7  score v%u (%s)", title, g_score.version, g_score.status);
    ImGui::PopFont();
    char t[32];
    score_text(t, sizeof(t), s->overall);
    ImGui::Text("%-18s %6s", "Overall", t);
    for (u32 c = 0; c < SC_COUNT; ++c) {
        u32 present, total;
        score_inputs_of((ScoreComp)c, &present, &total, s);
        score_text(t, sizeof(t), s->comp[c]);
        ImGui::Text("%-18s %6s", g_score_comp_names[c], t);
        if (present < total) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%u of %u inputs)", present, total);
        }
    }
    char flags[300];
    score_flag_text(s->flags, flags, sizeof(flags));
    if (flags[0]) ImGui::TextColored(g_pal.warn, "%s", flags);
    ImGui::TextDisabled("Reference machine = 1000. Higher is faster.");
    ImGui::EndTooltip();
}

// ---- About the score -------------------------------------------------------------------------------------

static void about_group_begin(const char* title) {
    theme_group_begin(title);
    ImGui::PushTextWrapPos(0.0f);       // wrap at the box's edge
}

static void about_group_end() {
    ImGui::PopTextWrapPos();
    theme_group_end();
}

// Bullet with wrapped text (ImGui::BulletText doesn't wrap); continuation lines line up with the first.
static void bullet(const char* text) {
    ImGui::Bullet();
    ImGui::TextUnformatted(text);
}

void score_ui_about(bool* request) {
    if (*request) {
        *request = false;
        ImGui::OpenPopup("About the score");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.8f, vp->WorkSize.y * 0.85f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("About the score", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const f32 fs = ImGui::GetFontSize();
    ImGui::BeginChild("##about", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y));
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::PushFont(g_fonts.medium, theme_font_size(1.2f));
    ImGui::Text("Score v%u  \xC2\xB7  %s", g_score.version, g_score.status);
    ImGui::PopFont();
    ImGui::TextUnformatted(
        "The raw numbers (with their units) are the real results. The score is a derived summary: computed only from "
        "those raw numbers by the published formula below, so any saved results file gets one, and always shown with "
        "its breakdown. Scores of different versions are never compared.");
    ImGui::Spacing();
    about_group_begin("Formula");
    bullet("Each input is a ratio to the reference machine: value / reference, or reference / value when lower "
           "is better (latency, frame time). > 1 means faster.");
    bullet("Component = 1000 x weighted geometric mean of its input ratios. The reference machine scores "
           "exactly 1000; 2000 = twice as fast on that mix.");
    bullet("Overall = 1000 x weighted geometric mean of the component ratios, only when every input is "
           "present (otherwise: component scores only, computed from the inputs that ran).");
    bullet("GPU inputs take the better of Vulkan and D3D12, so the API choice doesn't matter. Storage uses C: "
           "when it was tested, else the first tested drive.");
    ImGui::Spacing();
    ImGui::Text("Reference machine (= 1000): %s", g_score.reference_desc);
    ImGui::TextDisabled("Reference run: library/%s.toon.txt (frozen for this version)", g_score.reference);
    about_group_end();
    about_group_begin("How indicative is it?");
    ImGui::TextColored(g_pal.warn, "%s", g_score.calibration);
    ImGui::TextUnformatted("Correlation with real-task timings: not measured yet. Until then the score is labelled "
                           "preliminary.");
    about_group_end();
    about_group_begin("Overall (component weights, %)");
    if (ImGui::BeginTable("##weights", SC_COUNT, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) {
        for (u32 c = 0; c < SC_COUNT; ++c) ImGui::TableSetupColumn(g_score_comp_names[c]);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (u32 c = 0; c < SC_COUNT; ++c) {
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", g_score.overall_weights[c]);
        }
        ImGui::EndTable();
    }
    about_group_end();
    about_group_begin("Inputs");
    if (ImGui::BeginTable("##inputs_all", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Component");
        ImGui::TableSetupColumn("Input");
        ImGui::TableSetupColumn("Test / metric");
        ImGui::TableSetupColumn("Weight");
        ImGui::TableSetupColumn("Reference");
        ImGui::TableSetupColumn("Better");
        ImGui::TableHeadersRow();
        char r[32];
        for (u32 i = 0; i < g_score.input_count; ++i) {
            const ScoreInput* in = &g_score.inputs[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", g_score_comp_names[in->comp]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(in->label);
            ImGui::TableNextColumn();
            ImGui::Text("%s  %s%s", in->test, (in->flags & SI_DRIVE) ? "<drive>: " : "", in->metric);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", in->weight);
            ImGui::TableNextColumn();
            format_value(r, sizeof(r), in->ref);
            ImGui::Text("%s %s", r, in->unit);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", in->lower_better ? "lower" : "higher");
        }
        ImGui::EndTable();
    }
    about_group_end();
    about_group_begin("Validity flags (shown next to the score, never hidden)");
    for (u32 i = 0; i < SF_COUNT; ++i) bullet(g_score_flag_desc[i]);
    about_group_end();
    ImGui::TextDisabled("Details: docs/notes/score-results.md in the source repository.");
    (void)fs;
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    if (theme_button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---- Score tab ----------------------------------------------------------------------------------------

struct ScoreUi {
    ScoreResult cur;
    f64 cur_end;            // g_run.end_time the cached score belongs to
    u32 cur_done;
    bool have;
    bool about;
    char status[400];
    // ranking
    bool lib_loaded;
    u32 lib_count;
    ScoreResult* lib;       // per library run (VirtualAlloc'd once)
    i32 rank_metric;        // < SCORE_ITEM_COUNT: score item (score_item); else index into metric list + that
    u32 metric_count;
    struct Key { char test[48]; char name[64]; char unit[16]; f64 x; } metrics[1024];
    f64 lib_value[256];     // the selected metric per library run (NaN = not in that run)
    f64 cur_value;
    i32 lib_value_for;      // rank_metric the values above belong to, -1 = none
};
static ScoreUi s_su = { .rank_metric = 0, .lib_value_for = -1 };

static bool current_score(const ScoreResult** out) {
    if (runner_is_running() || g_run.total == 0) return false;
    const u32 done = g_run.done_count.load();
    if (!s_su.have || s_su.cur_end != g_run.end_time || s_su.cur_done != done) {
        score_current(&s_su.cur);
        s_su.cur_end = g_run.end_time;
        s_su.cur_done = done;
        s_su.have = true;
        s_su.lib_value_for = -1;
        s_su.metric_count = 0;
    }
    *out = &s_su.cur;
    return s_su.cur.any;
}

static void load_library_scores();

static void select_score_preset() {
    if (const RunPreset* pr = preset_find("score")) {
        bool* sel = ui_selection();
        preset_apply(pr, ui_params(), sel);
    }
}

void score_ui_draw() {
    const f32 fs = ImGui::GetFontSize();
    score_ui_about(&s_su.about);
    char msg[400];
    if (snapshot_take_status(SNAP_MAIN, msg, sizeof(msg))) snprintf(s_su.status, sizeof(s_su.status), "%s", msg);

    const ScoreResult* s = nullptr;
    const bool ok = current_score(&s);
    ImGui::BeginChild("##score", ImVec2(0, 0));
    if (!ok) {
        ImGui::Spacing();
        if (runner_is_running()) ImGui::TextDisabled("The score appears when the run finishes.");
        else ImGui::TextDisabled("No score yet: run the Score preset (the tests the score needs, about 5 minutes).");
        ImGui::Spacing();
        ImGui::BeginDisabled(runner_is_running());
        if (theme_button("Select the Score preset")) select_score_preset();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (theme_button("About the score")) s_su.about = true;
        ImGui::EndChild();
        return;
    }

    // Headline: overall score (or "partial"), version, reference.
    char t[32];
    score_text(t, sizeof(t), s->overall);
    const f32 y0 = ImGui::GetCursorPosY();
    ImGui::PushFont(g_fonts.medium, theme_font_size(2.6f));
    ImGui::TextColored(s->complete ? g_pal.primary : g_pal.on_surface_variant, "%s", s->complete ? t : "Partial");
    const f32 big_h = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ImGui::SameLine(0.0f, fs);
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(y0 + big_h * 0.08f);
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::Text("Overall score  \xC2\xB7  v%u", g_score.version);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(g_pal.warn, "%s", g_score.status);
    ImGui::TextDisabled(s->complete ? "Reference machine = 1000 (R7 250 + RTX 5060 Laptop). Higher is faster."
                                    : "Some score tests didn't run: component scores only.");
    ImGui::EndGroup();
    ImGui::SetCursorPosY(y0 + big_h + ImGui::GetStyle().ItemSpacing.y);

    char flags[300];
    score_flag_text(s->flags & ~SF_INCOMPLETE, flags, sizeof(flags));
    if (flags[0]) {
        ImGui::TextColored(g_pal.warn, "Flags: %s", flags);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (u32 i = 0; i < SF_COUNT; ++i)
                if ((s->flags & (1u << i)) && i) ImGui::BulletText("%s", g_score_flag_desc[i]);
            ImGui::EndTooltip();
        }
    }

    // Components: bar (0..2x the reference, marker at the reference), value; hover = inputs, click = expand.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    f64 hi = 2000.0;
    for (u32 c = 0; c < SC_COUNT; ++c)
        if (isfinite(s->comp[c]) && s->comp[c] * 1.1 > hi) hi = s->comp[c] * 1.1;
    f32 name_w = 0.0f;
    for (u32 c = 0; c < SC_COUNT; ++c) {
        const f32 w = ImGui::CalcTextSize(g_score_comp_names[c]).x;
        name_w = w > name_w ? w : name_w;
    }
    name_w += ImGui::GetCursorPosX() + ImGui::GetTreeNodeToLabelSpacing() + fs;
    const f32 value_w = ImGui::CalcTextSize("00000").x;
    for (u32 c = 0; c < SC_COUNT; ++c) {
        ImGui::PushID((int)c);
        u32 present, total;
        score_inputs_of((ScoreComp)c, &present, &total, s);
        theme_header_push(false);
        const bool open = ImGui::TreeNodeEx("##comp", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                                            "%s", g_score_comp_names[c]);
        theme_header_pop();
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SameLine(name_w);
        const f32 bar_w = ImGui::GetContentRegionAvail().x - value_w - fs * 6.0f;
        bar(isfinite(s->comp[c]) ? (f32)(s->comp[c] / hi) : 0.0f, (f32)(1000.0 / hi), bar_w > fs * 4 ? bar_w : fs * 4,
            fs * 0.55f, present == total ? g_pal.primary : g_pal.outline);
        ImGui::SameLine();
        score_text(t, sizeof(t), s->comp[c]);
        theme_push_numbers();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + value_w - ImGui::CalcTextSize(t).x);
        ImGui::TextUnformatted(t);
        ImGui::PopFont();
        ImGui::SameLine();
        if (present < total) ImGui::TextColored(g_pal.warn, "%u/%u inputs", present, total);
        else ImGui::TextDisabled("%u inputs", total);
        if (hovered && !open) {
            ImGui::BeginTooltip();
            ImGui::TextDisabled("%s: click to expand", g_score_comp_names[c]);
            score_ui_inputs_table(s, (ScoreComp)c);
            ImGui::EndTooltip();
        }
        if (open) {
            score_ui_inputs_table(s, (ScoreComp)c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (theme_button("About the score")) s_su.about = true;
    ImGui::SameLine();
    if (theme_button("Save PNG")) {
        wchar_t path[MAX_PATH];
        snapshot_export_path("score", L"png", path);
        snapshot_request(SNAP_MAIN, path);
    }
    theme_help("Saves a screenshot of this window as a PNG in the results folder.");
    if (!s->complete) {
        ImGui::SameLine();
        ImGui::BeginDisabled(runner_is_running());
        if (theme_button("Select the Score preset")) select_score_preset();
        ImGui::EndDisabled();
    }
    if (s_su.status[0]) ImGui::TextDisabled("%s", s_su.status);
    // vs library machines: place among the built-in runs (details on the Ranking tab).
    load_library_scores();
    u32 better = 0, ranked = 0;
    for (u32 i = 0; i < s_su.lib_count && s->complete; ++i) {
        if (!isfinite(s_su.lib[i].overall)) continue;
        ++ranked;
        better += s_su.lib[i].overall > s->overall;
    }
    if (ranked) ImGui::TextDisabled("Overall: #%u of %u among this run and the %u built-in library run%s. The Ranking tab "
                                    "has every score and test.", better + 1, ranked + 1, ranked, ranked == 1 ? "" : "s");
    else ImGui::TextDisabled("See the Ranking tab for this run against the built-in library.");
    ImGui::EndChild();
}

// ---- Ranking tab -------------------------------------------------------------------------------------------

static void load_library_scores() {
    if (s_su.lib_loaded) return;
    s_su.lib_loaded = true;
    s_su.lib_count = library_load();
    if (s_su.lib_count > ARRAY_COUNT(s_su.lib_value)) s_su.lib_count = ARRAY_COUNT(s_su.lib_value);
    if (!s_su.lib_count) return;
    s_su.lib = (ScoreResult*)VirtualAlloc(nullptr, sizeof(ScoreResult) * s_su.lib_count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_su.lib) { s_su.lib_count = 0; return; }
    for (u32 i = 0; i < s_su.lib_count; ++i) s_su.lib[i] = library_run(i)->info.score;
}

// Plain-value metrics of a run, for the "rank by" list.
static void build_metric_list(const RunFile* rf) {
    s_su.metric_count = 0;
    for (u32 i = 0; i < rf->metric_count && s_su.metric_count < ARRAY_COUNT(s_su.metrics); ++i) {
        const RfMetric* m = &rf->metrics[i];
        if (m->x != 0.0 || !strcmp(m->unit, "x") || !strcmp(m->unit, "%")) continue;   // series / matrix cells, ratios
        ScoreUi::Key* k = &s_su.metrics[s_su.metric_count++];
        snprintf(k->test, sizeof(k->test), "%s", rf->tests[m->test].id);
        snprintf(k->name, sizeof(k->name), "%s", m->name);
        snprintf(k->unit, sizeof(k->unit), "%s", m->unit);
        k->x = m->x;
    }
}

static f64 metric_value(const RunFile* rf, const ScoreUi::Key* k) {
    const i32 t = rf_find_test(rf, k->test);
    const i32 m = t >= 0 ? rf_find_metric(rf, (u32)t, k->name, k->x) : -1;
    return m >= 0 ? rf->metrics[m].value : NAN;
}

static void load_metric_values() {
    const i32 sel = s_su.rank_metric - (i32)SCORE_ITEM_COUNT;
    if (s_su.lib_value_for == s_su.rank_metric || sel < 0 || sel >= (i32)s_su.metric_count) return;
    s_su.lib_value_for = s_su.rank_metric;
    const ScoreUi::Key* k = &s_su.metrics[sel];
    RunFile* rf = rf_scratch();
    char err[64];
    for (u32 i = 0; i < s_su.lib_count; ++i) {
        const LibraryRun* r = library_run(i);
        s_su.lib_value[i] = rf_parse(r->data, r->size, rf, err, sizeof(err)) ? metric_value(rf, k) : NAN;
    }
    s_su.cur_value = NAN;
    if (g_run.total && !runner_is_running()) {
        rf_from_current(rf);
        s_su.cur_value = metric_value(rf, k);
    }
}

static f64 score_pick(const ScoreResult* s, i32 sel) {
    return score_item(s, (u32)sel);
}

static void rank_label(i32 sel, char* buf, u32 size) {
    if (sel < (i32)SCORE_ITEM_COUNT) snprintf(buf, size, "Score: %s", score_item_name((u32)sel));
    else {
        const ScoreUi::Key* k = &s_su.metrics[sel - SCORE_ITEM_COUNT];
        snprintf(buf, size, "%s  \xC2\xB7  %s (%s)", k->test, k->name, k->unit);
    }
}

struct RankEntry {
    const char* label;
    const char* sub;
    f64 value;
    bool current, reference;
};

static int cmp_entries(const void* a, const void* b) {
    const f64 va = ((const RankEntry*)a)->value, vb = ((const RankEntry*)b)->value;
    return va < vb ? 1 : va > vb ? -1 : 0;      // already in "higher = better" terms
}

void ranking_ui_draw() {
    load_library_scores();
    const ScoreResult* cur = nullptr;
    const bool have_cur = current_score(&cur) || (g_run.total && !runner_is_running());
    if (s_su.metric_count == 0) {
        RunFile* rf = rf_scratch();
        char err[64];
        bool any = true;
        if (have_cur) rf_from_current(rf);
        else any = s_su.lib_count && rf_parse(library_run(0)->data, library_run(0)->size, rf, err, sizeof(err));
        if (any) build_metric_list(rf);
        s_su.lib_value_for = -1;
    }
    if (s_su.rank_metric >= (i32)(SCORE_ITEM_COUNT + s_su.metric_count)) s_su.rank_metric = 0;
    static f64 defaulted_for = -1.0;     // once per run: don't fight the user's choice
    if (cur && cur->any && !cur->complete && s_su.rank_metric == 0 && defaulted_for != g_run.end_time) {
        defaulted_for = g_run.end_time;
        for (u32 c = 0; c < SC_COUNT; ++c)
            if (isfinite(cur->comp[c])) { s_su.rank_metric = (i32)(1 + c); break; }
    }

    const f32 fs = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Rank by");
    ImGui::SameLine();
    char label[200];
    rank_label(s_su.rank_metric, label, sizeof(label));
    ImGui::SetNextItemWidth(fs * 26.0f);
    if (ImGui::BeginCombo("##rank", label, ImGuiComboFlags_HeightLarge)) {
        static ImGuiTextFilter filter;
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); filter.Clear(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##rank_filter", "Search tests / metrics", filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf)))
            filter.Build();
        const i32 n = (i32)(SCORE_ITEM_COUNT + s_su.metric_count);
        for (i32 i = 0; i < n; ++i) {
            rank_label(i, label, sizeof(label));
            if (!filter.PassFilter(label)) continue;
            if (i == (i32)SCORE_ITEM_COUNT) ImGui::Separator();
            if (ImGui::Selectable(label, i == s_su.rank_metric)) s_su.rank_metric = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u library run%s", s_su.lib_count, s_su.lib_count == 1 ? "" : "s");

    // Entries: the current run + every library run, in "higher = better" order.
    static RankEntry entries[257];
    static char cur_sub[160];
    u32 n = 0;
    const i32 sel = s_su.rank_metric;
    const bool is_score = sel < (i32)SCORE_ITEM_COUNT;
    bool lower_better = false;
    const char* unit = "";
    if (!is_score) {
        load_metric_values();
        const ScoreUi::Key* k = &s_su.metrics[sel - SCORE_ITEM_COUNT];
        lower_better = unit_lower_is_better(k->unit);
        unit = k->unit;
    }
    if (have_cur) {
        const f64 v = is_score ? (cur ? score_pick(cur, sel) : NAN) : s_su.cur_value;
        char c[48], g[48];
        cpu_short_name(g_sys.cpu.brand, c, sizeof(c));
        gpu_short_name(g_run.params.gpu_adapter < g_sys.gpu_count ? g_sys.gpus[g_run.params.gpu_adapter].name : "", g, sizeof(g));
        snprintf(cur_sub, sizeof(cur_sub), "%s, %s", c, g);
        if (isfinite(v)) entries[n++] = { "This run", cur_sub, v, true, false };
    }
    for (u32 i = 0; i < s_su.lib_count; ++i) {
        const LibraryRun* r = library_run(i);
        const f64 v = is_score ? score_pick(&s_su.lib[i], sel) : s_su.lib_value[i];
        if (!isfinite(v)) continue;
        entries[n++] = { r->name, r->info.info[CI_CPU], v, false, !strcmp(r->name, g_score.reference) };
    }
    for (u32 i = 0; i < n; ++i) if (lower_better) entries[i].value = -entries[i].value;
    qsort(entries, n, sizeof(entries[0]), cmp_entries);
    for (u32 i = 0; i < n; ++i) if (lower_better) entries[i].value = -entries[i].value;

    ImGui::Spacing();
    if (!n) {
        ImGui::TextDisabled(is_score ? "No scores to rank: run the Score preset, or add runs to library/."
                                     : "No run has this metric.");
        return;
    }
    // Bars relative to the best entry (performance terms: lower-is-better metrics are inverted).
    f64 best = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const f64 perf = lower_better ? 1.0 / entries[i].value : entries[i].value;
        if (perf > best) best = perf;
    }
    ImGui::BeginChild("##ranking", ImVec2(0, 0));
    const f32 avail_w = ImGui::GetContentRegionAvail().x;
    const f32 label_w = avail_w * 0.38f < fs * 15.0f ? avail_w * 0.38f : fs * 15.0f, value_w = fs * 7.0f;
    char v[32];
    for (u32 i = 0; i < n; ++i) {
        const RankEntry* e = &entries[i];
        ImGui::PushID((int)i);
        ImGui::BeginGroup();
        ImGui::TextDisabled("%2u", i + 1);
        ImGui::SameLine();
        // Long run names are clipped at the bar; the full name is on hover.
        const ImVec2 lp = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(lp, ImVec2(ImGui::GetWindowPos().x + label_w - fs * 0.5f, lp.y + ImGui::GetTextLineHeight()), true);
        if (e->current) ImGui::TextColored(g_pal.primary, "%s", e->label);
        else ImGui::TextUnformatted(e->label);
        ImGui::PopClipRect();
        ImGui::SameLine(label_w);
        const f64 perf = lower_better ? 1.0 / e->value : e->value;
        const f32 w = ImGui::GetContentRegionAvail().x - value_w;
        bar((f32)(perf / best), 0.0f, w > fs * 4 ? w : fs * 4, fs * 0.7f, e->current ? g_pal.primary : g_pal.outline);
        ImGui::SameLine();
        if (is_score) snprintf(v, sizeof(v), "%.0f", e->value);
        else format_value(v, sizeof(v), e->value);
        theme_push_numbers();
        ImGui::Text("%s", v);
        ImGui::PopFont();
        if (!is_score) { ImGui::SameLine(); ImGui::TextDisabled("%s", unit); }
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s%s%s", e->label, e->sub, e->reference ? "\nScore reference machine (= 1000)" : "",
                              lower_better ? "\nLower is better; bars show speed (inverted)." : "");
        ImGui::PopID();
    }
    if (s_su.lib_count <= 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.on_surface_variant);
        ImGui::TextWrapped("The library has %u run so far. Add more by dropping *.toon.txt files into library/ and "
                           "rebuilding, or share yours (see the website).", s_su.lib_count);
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}
