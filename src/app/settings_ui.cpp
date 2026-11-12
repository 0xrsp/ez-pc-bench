// Settings window: one row per setting (label | control) in titled groups; every row has a detailed
// tooltip. Edits go straight into the main window's BenchParams (ui_params) and are saved by settings.cpp.
#include "app/settings_ui.h"
#include "app/theme.h"
#include "app/ui.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "disk/disk.h"
#include "imgui.h"
#include "imgui_internal.h"     // TableGetHoveredRow
#include <windows.h>
#include <stdio.h>

static bool begin_group(const char* title) {
    ImGui::Spacing();
    ImGui::PushFont(g_fonts.medium, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    // A little vertical cell padding so rows are easy hover targets.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize() * 0.2f));
    const bool ok = ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp);
    ImGui::PopStyleVar();
    if (!ok) return false;
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
    const bool hovered = ImGui::IsWindowHovered() &&
                         ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex() && !ImGui::IsAnyItemActive() &&
                         !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (hovered) {
        const ImVec4 t = g_pal.on_surface;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(t.x, t.y, t.z, 0.05f)));
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

    static const char* const datas[] = { "Random (incompressible)", "All zeros (0x00)" };
    row("Test data", "What the test file and the writes contain. Random data can't be compressed, which is what real files "
                     "mostly look like. All zeros shows whether the drive's controller compresses or deduplicates data: "
                     "such drives get much faster with it (most current consumer SSDs don't).");
    int data = o->data;
    if (ImGui::Combo("##ddata", &data, datas, (int)ARRAY_COUNT(datas))) o->data = (u8)data;

    static const char* const units[] = { "MB/s", "GB/s", "IOPS", "\xC2\xB5s (latency)" };
    row("Disk results shown as", "Which number the test tree shows for each disk test: throughput in MB/s or GB/s, IOPS, or "
                                 "the average latency in microseconds. GB/s also converts MB/s values in the results "
                                 "table. Saved results always contain all of them.");
    int unit = o->unit;
    if (ImGui::Combo("##dunit", &unit, units, (int)ARRAY_COUNT(units))) o->unit = (u8)unit;

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

// Text fields for the four user-defined rows (disk.custom1..4).
static void draw_disk_rows(BenchParams* p) {
    static char text[DISK_CUSTOM_ROWS][48];
    static bool valid[DISK_CUSTOM_ROWS], init;
    if (!init) {
        for (u32 i = 0; i < DISK_CUSTOM_ROWS; ++i) {
            disk_row_format(&p->disk.custom[i], text[i], sizeof(text[i]));
            valid[i] = true;
        }
        init = true;
    }
    const char* help = "A test row of your own, run by the Disk > Custom tests in the main window (tick them there). Format: "
                       "SEQ or RND, block size, queue depth and threads, optionally R and the share of reads: "
                       "\"RND4K Q32T4\" runs a read pass and a write pass like the built-in rows, \"RND4K Q32T4 R70\" one "
                       "mixed pass with 70 % reads, R100 reads only, R0 writes only. Sizes 512 B to 64M, QD 1-256, "
                       "threads 1-64. Empty = row unused.";
    for (u32 i = 0; i < DISK_CUSTOM_ROWS; ++i) {
        char label[16];
        snprintf(label, sizeof(label), "Row %u", i + 1);
        row(label, help);
        ImGui::PushID((int)i);
        if (!valid[i]) ImGui::PushStyleColor(ImGuiCol_Text, g_pal.error);
        if (ImGui::InputTextWithHint("##row", "e.g. RND4K Q32T4 R70", text[i], sizeof(text[i]))) {
            DiskRow r;
            valid[i] = disk_row_parse(text[i], &r);
            if (valid[i]) p->disk.custom[i] = r;
        }
        if (!valid[i]) ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void settings_ui_draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    BenchParams* p = ui_params();
    const bool running = runner_is_running();
    ImGui::TextDisabled("Hover a setting for details. Changes apply to the next run and are saved automatically.");

    if (begin_group("Appearance")) {
        static const char* const modes[] = { "Follow Windows", "Light", "Dark" };
        const char* help = "Follow Windows uses the light / dark app mode from Windows Settings > Personalization > Colors "
                           "and switches live when you change it. Light or Dark keeps that look regardless of Windows.";
        row("Theme", help);
        int mode = (int)theme_mode();
        if (ImGui::Combo("##theme", &mode, modes, (int)ARRAY_COUNT(modes))) theme_set_mode((ThemeMode)mode);
        end_group();
    }

    if (running) {
        ImGui::Spacing();
        ImGui::TextColored(g_pal.warn, "A run is in progress: the settings below are locked until it finishes.");
    }
    ImGui::BeginDisabled(running);

    if (begin_group("Benchmark")) {
        const char* help = "How long each measured phase runs, e.g. the single-thread and multi-thread halves of a CPU "
                           "test or each disk pass. A short warm-up comes on top. Longer phases average out boost-clock "
                           "and temperature swings but make a full run take longer. 3000 ms is the default and what the "
                           "reference runs use, so keep it when comparing against them.";
        row("Duration per phase", help);
        int duration = (int)p->duration_ms;
        if (ImGui::SliderInt("##duration", &duration, 500, 20000, "%d ms")) p->duration_ms = (u32)duration;

        char threads_help[400];
        snprintf(threads_help, sizeof(threads_help),
                 "Threads used by the multi-threaded CPU and memory tests. 0 uses every logical processor (%u on this "
                 "PC). Lower values show how throughput scales with cores; single-thread results are not affected.",
                 g_sys.cpu.threads);
        row("Threads", threads_help);
        int threads = (int)p->thread_count;
        if (ImGui::SliderInt("##threads", &threads, 0, (int)g_sys.cpu.threads, threads ? "%d" : "all"))
            p->thread_count = (u32)threads;

        const char* repeat_help = "Runs every test this many times in a row and reports the median, with the spread "
                                  "(min, max, coefficient of variation) on hover. Values whose runs differ by more than "
                                  "3 % are marked as noisy. 1 = a single run (default).";
        row("Repeat each test", repeat_help);
        int repeat = (int)p->repeat;
        if (ImGui::SliderInt("##repeat", &repeat, 1, (int)MAX_REPEAT, repeat > 1 ? "%dx (median)" : "once"))
            p->repeat = (u32)repeat;
        end_group();
    }

    if (begin_group("GPU")) {
        const char* help = "Size of the offscreen image the 3D and 2D GPU tests render into. Higher resolutions load "
                           "pixel fill and memory bandwidth harder, so frame times and fill rates change with it: only "
                           "compare runs made at the same resolution. Default 1920 x 1080.";
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

        const char* wt_help = "Opens the test file with write-through (FUA): every write must reach the drive's permanent "
                              "storage before it counts, bypassing its volatile write cache. Shows real persistent write "
                              "speed; most drives are much slower this way. Off by default, like most benchmarks.";
        row("Write-through (FUA)", wt_help);
        theme_checkbox("##wt", &p->disk_write_through);
        draw_disk_options(p);
        end_group();
    }

    if (begin_group("Custom disk tests")) {
        draw_disk_rows(p);
        end_group();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    if (theme_button("Reset to defaults")) {
        const BenchParams d;            // keeps the GPU and disk(s) picked in the main window
        p->duration_ms = d.duration_ms;
        p->thread_count = d.thread_count;
        p->gpu_width = d.gpu_width;
        p->gpu_height = d.gpu_height;
        p->disk_file_bytes = d.disk_file_bytes;
        p->disk_passes = d.disk_passes;
        p->disk_write_through = d.disk_write_through;
        p->repeat = d.repeat;
        // Disk options; the folder target and the custom rows stay (they are definitions, not tuning).
        const DiskOptions keep = p->disk;
        p->disk = d.disk;
        wcscpy_s(p->disk.folder, keep.folder);
        memcpy(p->disk.custom, keep.custom, sizeof(keep.custom));
    }
    theme_help("Restores the benchmark, GPU and disk settings above to their defaults. The theme and the GPU / disks "
               "picked in the main window stay as they are.");
    ImGui::EndDisabled();
    ImGui::End();
}
