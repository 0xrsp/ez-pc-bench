// Settings and Benchmark settings windows: one row per setting (label | control) in titled groups; every row
// has a detailed tooltip. Edits go straight into the main window's BenchParams (ui_params) and are saved by
// settings.cpp (one ini for both windows).
#include "app/settings_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/presets.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "imgui.h"
#include "imgui_internal.h"     // TableGetHoveredRow, OpenPopupEx (dev: --open-theme-combo)
#include <windows.h>
#include <stdio.h>

static bool begin_group(const char* title) {
    theme_group_begin(title);
    // A little vertical cell padding so rows are easy hover targets.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize() * 0.2f));
    const bool ok = ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp);
    ImGui::PopStyleVar();
    if (!ok) {
        theme_group_end();
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

// Help of the row being drawn: shown when the mouse is anywhere on that row (label, control or the space
// between), after a short delay, unless a control is being dragged or a dropdown is open.
static const char* s_row_help;
static ImGuiID s_hover_id;
static f64 s_hover_since;

static void finish_row() {
    if (!s_row_help) return;
    const ImGuiID id = ImGui::GetID(s_row_help);
    const bool hovered = ImGui::IsWindowHovered() &&     // (AllowWhenDisabled is an item flag; ImGui rejects it here)
                         ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex() && !ImGui::IsAnyItemActive() &&
                         !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (hovered) {
        if (s_hover_id != id) { s_hover_id = id; s_hover_since = ImGui::GetTime(); }
        if (ImGui::GetTime() - s_hover_since > 0.25) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
            ImGui::TextUnformatted(s_row_help);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    } else if (s_hover_id == id) {
        s_hover_id = 0;
    }
    s_row_help = nullptr;
}

static void end_group() {
    finish_row();
    ImGui::EndTable();
    theme_group_end();
}

// Starts a row: label, then the control column with a full-width item. `help` covers the whole row.
static void row(const char* label, const char* help) {
    finish_row();
    ImGui::TableNextRow();
    s_row_help = help;
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static u32 s_open_theme_combo;   // dev (--open-theme-combo): frame counter until the theme dropdown opens itself

void settings_ui_open_theme_combo() { s_open_theme_combo = 1; }

// Theme dropdown: a color swatch before each name; the named schemes below a separator.
static void theme_combo() {
    const ThemeMode cur = theme_mode();
    const f32 h = ImGui::GetTextLineHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
    if (s_open_theme_combo && ++s_open_theme_combo == 4) {
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID("##theme")));
        s_open_theme_combo = 0;
    }
    if (ImGui::BeginCombo("##theme", "", ImGuiComboFlags_HeightLargest)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, h * 0.35f));
        for (u32 i = 0; i < theme_count(); ++i) {
            const ThemeMode t = (ThemeMode)i;
            if (t == THEME_DARK + 1) ImGui::Separator();
            ImGui::PushID((int)i);
            if (ImGui::Selectable("##t", t == cur)) theme_set_mode(t);
            if (t == cur) ImGui::SetItemDefaultFocus();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // The item rect includes half the item spacing on each side: center swatch and name vertically in it,
            // start them where a plain Selectable's text would.
            const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            const ImVec2 p(r0.x + floorf(ImGui::GetStyle().ItemSpacing.x * 0.5f), floorf((r0.y + r1.y - h) * 0.5f));
            const f32 w = theme_swatch(dl, p, h, t);
            dl->AddText(ImVec2(p.x + w + gap, p.y), ImGui::GetColorU32(ImGuiCol_Text), theme_name(t));
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }
    if (ImGui::BeginComboPreview()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const f32 w = theme_swatch(dl, p, h, cur);
        dl->AddText(ImVec2(p.x + w + gap, p.y), ImGui::GetColorU32(ImGuiCol_Text), theme_name(cur));
        ImGui::EndComboPreview();
    }
}

// Disk options beyond file size / passes / write-through (DiskOptions in core/bench.h).
static void draw_disk_options(BenchParams* p) {
    DiskOptions* o = &p->disk;
    static const char* const modes[] = { "Read + write", "Read only", "Write only", "Read + write + mix" };
    row("Read / write passes", "Which passes the CrystalDiskMark-style rows run, like CrystalDiskMark's Read&Write / Read / "
                               "Write / +Mix menu. Read only never writes to the drive (after the test file is created). "
                               "+ mix adds a mixed pass with the read share below, e.g. 70 % reads / 30 % writes. Also "
                               "applies to the sweeps and the latency distribution.");
    int mode = o->mode;
    if (ImGui::Combo("##dmode", &mode, modes, (int)ARRAY_COUNT(modes))) o->mode = (u8)mode;

    row("Mixed pass: reads", "Share of reads in the mixed pass (Read + write + mix). Each request is a read or a write at "
                             "random with this ratio, like CrystalDiskMark's Mix (default 70 %).");
    int mix = o->mix_read_pct;
    if (ImGui::SliderInt("##dmix", &mix, 0, 100, "%d %% reads")) o->mix_read_pct = (u8)mix;

    static const char* const units[] = { "MB/s", "GB/s", "IOPS", "\xC2\xB5s (latency)" };
    row("Disk results shown as", "Which number the test tree shows for each disk test: throughput in MB/s or GB/s, IOPS, or "
                                 "the average latency in microseconds. GB/s also converts MB/s values in the results "
                                 "table. Saved results always contain all of them.");
    int unit = o->unit;
    if (ImGui::Combo("##dunit", &unit, units, (int)ARRAY_COUNT(units))) o->unit = (u8)unit;

    char label[32];
    row("Test in folder", "Run the disk suite in this folder instead of on the drive letters picked in the main window: a "
                          "network share (\\\\server\\share), a USB stick, a mounted VHD or a RAM disk. Metrics are then named "
                          "\"Folder: ...\". The raw-device tests use the drive letter of the folder's volume, if it has "
                          "one. Empty = test the drives (default).");
    static char folder[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, o->folder, -1, folder, sizeof(folder), nullptr, nullptr);
    if (ImGui::InputTextWithHint("##dfolder", "off: the drives picked in the main window", folder, sizeof(folder)))
        MultiByteToWideChar(CP_UTF8, 0, folder, -1, o->folder, (int)ARRAY_COUNT(o->folder));

    row("Buffered I/O", "Go through the Windows file cache instead of bypassing it. Off (default) measures the drive itself, "
                        "like CrystalDiskMark. On shows what programs see when they read files they used recently (served "
                        "from RAM), and is needed for some network shares that refuse unbuffered I/O.");
    theme_checkbox("##dbuf", &o->buffered);

    static const u32 caps[] = { 2, 8, 16, 32, 64, 128, 256, 512, 1024 };
    row("Sustained write: amount", "How much the Sustained write (SLC cache) test writes per drive, capped at 80 % of the free "
                                   "space. To find where the SLC cache runs out it has to write more than the cache holds: "
                                   "on an empty 1 TB SSD that can be 100 GB or more. The test is off by default and asks "
                                   "before it runs. Default 64 GiB.");
    snprintf(label, sizeof(label), "%u GiB", o->sustained_gib);
    if (ImGui::BeginCombo("##dsus", label)) {
        for (u32 i = 0; i < ARRAY_COUNT(caps); ++i) {
            snprintf(label, sizeof(label), "%u GiB", caps[i]);
            if (ImGui::Selectable(label, o->sustained_gib == caps[i])) o->sustained_gib = caps[i];
        }
        ImGui::EndCombo();
    }
}

// Pause before each disk test (DiskOptions::interval_ms); in the benchmark settings.
static void draw_disk_interval(BenchParams* p) {
    DiskOptions* o = &p->disk;
    static const u32 intervals[] = { 0, 1, 3, 5, 10, 30, 60, 180, 300, 600 };
    row("Interval between tests", "Pause before each disk test so the drive can cool down and finish background work "
                                  "(CrystalDiskMark's Interval). Laptop SSDs throttle when hot; a pause makes later tests "
                                  "comparable with earlier ones. 0 = none (default).");
    char label[32];
    snprintf(label, sizeof(label), o->interval_ms ? "%u s" : "none", o->interval_ms / 1000);
    if (ImGui::BeginCombo("##dint", label)) {
        for (u32 i = 0; i < ARRAY_COUNT(intervals); ++i) {
            snprintf(label, sizeof(label), intervals[i] ? "%u s" : "none", intervals[i]);
            if (ImGui::Selectable(label, o->interval_ms == intervals[i] * 1000)) o->interval_ms = intervals[i] * 1000;
        }
        ImGui::EndCombo();
    }
}

// Full-window ImGui window of a settings tool window, with the wrapped intro line(s).
static void begin_window(const char* id, const char* intro) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", intro);
    ImGui::PopTextWrapPos();
}

static void locked_note(bool running) {
    if (!running) return;
    ImGui::Spacing();
    ImGui::TextColored(g_pal.warn, "A run is in progress: the settings below are locked until it finishes.");
}

void settings_ui_draw() {
    begin_window("##settings", "Hover a setting for details. Changes apply to the next run and are saved automatically. "
                               "Phase duration, repeat, sustained mode, the GPU render resolution and the disk passes "
                               "are in Benchmark settings (next to Run).");
    BenchParams* p = ui_params();
    const bool running = runner_is_running();

    if (begin_group("Appearance")) {
        const char* help = "Follow Windows uses plain Light or Dark to match the app mode in Windows Settings > "
                           "Personalization > Colors and switches live when you change it. Light and Dark are plain neutral "
                           "themes; the others are popular color schemes (Solarized, Monokai, Dracula, Nord, ...). Graph "
                           "lines use each theme's own hues.";
        row("Theme", help);
        theme_combo();
        end_group();
    }

    locked_note(running);
    ImGui::BeginDisabled(running);

    if (begin_group("CPU and memory")) {
        char threads_help[400];
        snprintf(threads_help, sizeof(threads_help),
                 "Threads used by the multi-threaded CPU and memory tests. 0 uses every logical processor (%u on this "
                 "PC). Lower values show how throughput scales with cores; single-thread results are not affected.",
                 g_sys.cpu.threads);
        row("Threads", threads_help);
        int threads = (int)p->thread_count;
        if (ImGui::SliderInt("##threads", &threads, 0, (int)g_sys.cpu.threads, threads ? "%d" : "all"))
            p->thread_count = (u32)threads;
        end_group();
    }

    if (begin_group("GPU")) {
        const char* present_help = "Where the 3D and 2D tests show their frames. Offscreen (default) renders into an "
                                   "image that never reaches the screen: clean GPU throughput, no compositor or vsync in "
                                   "the way. On-screen opens a benchmark window (or covers the screen) and presents every "
                                   "frame through a real swapchain with vsync off and tearing allowed, like a game; frame "
                                   "times then include presentation, and where the driver reports it you also get "
                                   "displayed-frame times and display latency. On laptops whose screen is driven by the "
                                   "integrated GPU, frames from the dedicated GPU are copied across to it. Esc stops a test.";
        row("Present mode", present_help);
        static const char* const present_modes[] = { "Offscreen", "On-screen, windowed", "On-screen, fullscreen" };
        int present = !p->gpu_onscreen ? 0 : p->gpu_fullscreen ? 2 : 1;
        if (ImGui::Combo("##present", &present, present_modes, (int)ARRAY_COUNT(present_modes))) {
            p->gpu_onscreen = present != 0;
            if (present) p->gpu_fullscreen = present == 2;
        }
        end_group();
    }

    if (begin_group("Disk")) {
        const char* wt_help = "Opens the test file with write-through (FUA): every write must reach the drive's permanent "
                              "storage before it counts, bypassing its volatile write cache. Shows real persistent write "
                              "speed; most drives are much slower this way. Off by default, like most benchmarks.";
        row("Write-through (FUA)", wt_help);
        theme_checkbox("##wt", &p->disk_write_through);
        draw_disk_options(p);
        end_group();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    if (theme_button("Reset to defaults")) {
        const BenchParams d;            // keeps the GPU and disk(s) picked in the main window
        p->thread_count = d.thread_count;
        p->gpu_onscreen = d.gpu_onscreen;
        p->gpu_fullscreen = d.gpu_fullscreen;
        p->disk_write_through = d.disk_write_through;
        // Disk options; the folder target stays (it is a definition, not tuning), and the interval belongs to the
        // benchmark settings.
        const DiskOptions keep = p->disk;
        p->disk = d.disk;
        wcscpy_s(p->disk.folder, keep.folder);
        p->disk.interval_ms = keep.interval_ms;
    }
    theme_help("Restores the CPU, GPU and disk settings above to their defaults. The theme, the benchmark settings "
               "(render resolution included) and the GPU / disks picked in the main window stay as they are.");
    ImGui::EndDisabled();
    ImGui::End();
}

// Benchmark settings: exactly what presets set (preset_apply, core/presets.cpp) plus the disk interval and the GPU
// render resolution, which presets leave alone.
void bench_settings_ui_draw() {
    begin_window("##bench_settings", "Hover a setting for details. Changes apply to the next run and are saved "
                                     "automatically.");
    BenchParams* p = ui_params();
    const bool running = runner_is_running();
    const RunPreset* cur = preset_match(p, ui_selection());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text("Presets change the run and disk values: picking one in the main window's Preset box sets them, and "
                "changing one here turns the preset into \"Custom\" (the render resolution and disk interval are never "
                "changed by a preset). Current preset: %s.", cur ? cur->name : "Custom");
    ImGui::PopTextWrapPos();
    locked_note(running);
    ImGui::BeginDisabled(running);

    if (begin_group("Run")) {
        const char* help = "How long each measured phase runs, e.g. the single-thread and multi-thread halves of a CPU "
                           "test or each disk pass. A short warm-up comes on top. Longer phases average out boost-clock "
                           "and temperature swings but make a full run take longer. 3000 ms is the default and what the "
                           "reference runs use, so keep it when comparing against them.";
        row("Duration per phase", help);
        int duration = (int)p->duration_ms;
        if (ImGui::SliderInt("##duration", &duration, 500, 20000, "%d ms")) p->duration_ms = (u32)duration;

        const char* repeat_help = "Runs every test this many times in a row and reports the median, with the spread "
                                  "(min, max, coefficient of variation) on hover. Values whose runs differ by more than "
                                  "3 % are marked as noisy. 1 = a single run (default).";
        row("Repeat each test", repeat_help);
        int repeat = (int)p->repeat;
        if (ImGui::SliderInt("##repeat", &repeat, 1, (int)MAX_REPEAT, repeat > 1 ? "%dx (median)" : "once"))
            p->repeat = (u32)repeat;

        const char* sustain_help = "Sustained (thermal) mode, like Cinebench's minimum test duration: every selected test "
                                   "loops for this many minutes (at least two passes) and reports the median, plus the "
                                   "first and last pass, min, max and the drop from the first to the last pass. The Charts "
                                   "tab plots every pass, which shows boost decay, power limits and thermal throttling. "
                                   "Overrides \"Repeat each test\". Off by default.";
        row("Sustained mode", sustain_help);
        int sustain_min = (int)((p->sustain_s + 59) / 60);
        if (ImGui::SliderInt("##sustain", &sustain_min, 0, 60, sustain_min ? "%d min per test" : "off"))
            p->sustain_s = (u32)sustain_min * 60;
        end_group();
    }

    if (begin_group("GPU")) {
        const char* help = "Size of the image the 3D and 2D GPU tests render into. Higher resolutions load pixel fill and "
                           "memory bandwidth harder, so frame times and fill rates change with it: only compare runs made "
                           "at the same resolution. Presets don't change it. Default 1920 x 1080.";
        row("Render resolution", help);
        int res[2] = { (int)p->gpu_width, (int)p->gpu_height };
        if (ImGui::InputInt2("##res", res)) {
            p->gpu_width = (u32)(res[0] < 64 ? 64 : res[0] > 7680 ? 7680 : res[0]);
            p->gpu_height = (u32)(res[1] < 64 ? 64 : res[1] > 4320 ? 4320 : res[1]);
        }
        end_group();
    }

    if (begin_group("Disk")) {
        const char* file_help = "Size of the temporary test file created on each tested drive. Larger files reach past the "
                                "SSD's DRAM and SLC caches and show sustained speed; small files finish sooner but may only "
                                "measure the cache. The file is deleted when the test ends, even after a crash. Default 1 GiB.";
        row("Test file size", file_help);
        static const u32 sizes_mib[] = { 64, 256, 512, 1024, 2048, 4096, 8192 };
        char size_label[16];
        snprintf(size_label, sizeof(size_label), "%llu MiB", p->disk_file_bytes / MiB);
        if (ImGui::BeginCombo("##file", size_label)) {
            for (u32 i = 0; i < ARRAY_COUNT(sizes_mib); ++i) {
                snprintf(size_label, sizeof(size_label), "%u MiB", sizes_mib[i]);
                if (ImGui::Selectable(size_label, p->disk_file_bytes == sizes_mib[i] * MiB)) p->disk_file_bytes = sizes_mib[i] * MiB;
            }
            ImGui::EndCombo();
        }

        const char* passes_help = "Each disk test runs this many read passes and write passes and reports the best one, like "
                                  "CrystalDiskMark. More passes give steadier numbers but write more to the SSD: a full "
                                  "default run writes roughly 27 GB per pass. Default 3.";
        row("Passes", passes_help);
        int passes = (int)p->disk_passes;
        if (ImGui::SliderInt("##passes", &passes, 1, 9)) p->disk_passes = (u32)passes;
        draw_disk_interval(p);
        end_group();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    if (theme_button("Reset to defaults")) {
        const BenchParams d;
        p->duration_ms = d.duration_ms;
        p->repeat = d.repeat;
        p->sustain_s = d.sustain_s;
        p->gpu_width = d.gpu_width;
        p->gpu_height = d.gpu_height;
        p->disk_file_bytes = d.disk_file_bytes;
        p->disk_passes = d.disk_passes;
        p->disk.interval_ms = d.disk.interval_ms;
    }
    theme_help("Restores the run, GPU and disk settings above to their defaults (the Standard preset's values, 1920 x "
               "1080). The test selection and the settings in the Settings window stay as they are.");
    ImGui::EndDisabled();
    ImGui::End();
}
