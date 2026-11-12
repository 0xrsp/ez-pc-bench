// Settings persistence. Each frame the UI state is compared with the previous frame's snapshot; a change is
// written to the ini one second after the last edit ([settings] and [tests] sections, rewritten whole).
#include "app/settings.h"
#include "core/adv_sensors.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

struct Snapshot {
    BenchParams params;
    ThemeMode theme = THEME_SYSTEM;
    bool adv_sensors = true;                    // PawnIO advanced sensors switch (core/adv_sensors.h)
    bool selected[MAX_BENCHES];
};

static wchar_t s_dir[MAX_PATH], s_path[MAX_PATH];
static Snapshot s_file;                         // what the ini holds (defaults where unset)
static Snapshot s_last;                         // state at the previous tick
static bool s_started, s_dirty;
static f64 s_dirty_since;
// Command-line overrides (--duration, --light/--dark, --only, --gpu-present): the ini keeps its value until the
// user changes it.
static bool s_keep_duration, s_keep_theme, s_keep_selection, s_keep_present;
static bool s_start_onscreen, s_start_fullscreen;
static u32 s_start_duration;
static ThemeMode s_start_theme;
static bool s_start_selected[MAX_BENCHES];

static const wchar_t* const THEME_NAMES[] = { L"system", L"light", L"dark" };

static const wchar_t* ini_path() {
    if (!s_path[0]) {
        PWSTR appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
            swprintf(s_dir, MAX_PATH, L"%s\\ez-pc-bench", appdata);
            swprintf(s_path, MAX_PATH, L"%s\\settings.ini", s_dir);
        }
        CoTaskMemFree(appdata);
    }
    return s_path;
}

// Creates the folder, and the file with a UTF-16 BOM so the profile API writes Unicode.
static bool ensure_file() {
    if (!ini_path()[0]) return false;
    CreateDirectoryW(s_dir, nullptr);
    HANDLE f = CreateFileW(s_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const u16 bom = 0xFEFF;
        DWORD written;
        WriteFile(f, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(f);
    }
    return GetFileAttributesW(s_path) != INVALID_FILE_ATTRIBUTES;
}

const wchar_t* settings_ini_file() { return ensure_file() ? s_path : nullptr; }

static u32 read_u32(const wchar_t* key, u32 def, u32 lo, u32 hi) {
    const UINT v = GetPrivateProfileIntW(L"settings", key, (INT)def, ini_path());
    return v >= lo && v <= hi ? v : def;
}

static bool read_str(const wchar_t* key, wchar_t* out, u32 size) {
    out[0] = 0;
    return GetPrivateProfileStringW(L"settings", key, L"", out, size, ini_path()) > 0;
}

ThemeMode settings_load_theme() {
    wchar_t v[16];
    if (ini_path()[0] && read_str(L"theme", v, ARRAY_COUNT(v)))
        for (u32 i = 0; i < ARRAY_COUNT(THEME_NAMES); ++i)
            if (!_wcsicmp(v, THEME_NAMES[i])) s_file.theme = (ThemeMode)i;
    return s_file.theme;
}

void settings_load(BenchParams* p, bool* selected) {
    if (ini_path()[0]) {
        p->duration_ms = read_u32(L"duration_ms", p->duration_ms, 100, 600000);
        p->thread_count = read_u32(L"threads", p->thread_count, 0, g_sys.cpu.threads);
        wchar_t w[256];
        char name[256];
        if (read_str(L"gpu", w, ARRAY_COUNT(w))) {     // stored by name: adapter order can change
            WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof(name), nullptr, nullptr);
            for (u32 i = 0; i < g_sys.gpu_count; ++i)
                if (!strcmp(g_sys.gpus[i].name, name)) p->gpu_adapter = i;
        }
        p->gpu_width = read_u32(L"gpu_width", p->gpu_width, 64, 16384);
        p->gpu_height = read_u32(L"gpu_height", p->gpu_height, 64, 16384);
        p->gpu_fullscreen = read_u32(L"gpu_fullscreen", p->gpu_fullscreen, 0, 1) != 0;
        p->gpu_onscreen = read_u32(L"gpu_onscreen", p->gpu_onscreen, 0, 1) != 0;
        if (read_str(L"disk_drives", w, ARRAY_COUNT(w))) {      // "C,D"; drives that aren't fixed any more are dropped
            u32 mask = 0;
            for (const wchar_t* c = w; *c; ++c) {
                const wchar_t root[4] = { towupper(*c), L':', L'\\', 0 };
                if (root[0] >= L'A' && root[0] <= L'Z' && GetDriveTypeW(root) == DRIVE_FIXED) mask |= 1u << (root[0] - L'A');
            }
            if (mask) p->disk_drives = mask;
        }
        p->disk_file_bytes = read_u32(L"disk_file_mib", (u32)(p->disk_file_bytes / MiB), 16, 1024 * 1024) * MiB;
        p->disk_passes = read_u32(L"disk_passes", p->disk_passes, 1, 99);
        p->disk_write_through = read_u32(L"disk_write_through", p->disk_write_through, 0, 1) != 0;
        p->repeat = read_u32(L"repeat", p->repeat, 1, MAX_REPEAT);
        adv_set_enabled(read_u32(L"adv_sensors", 1, 0, 1) != 0);
        s_file.adv_sensors = adv_enabled();

        // [tests] id=0|1; ids this build doesn't know are ignored, missing ids keep the registry default.
        static wchar_t buf[32768];
        const DWORD n = GetPrivateProfileSectionW(L"tests", buf, ARRAY_COUNT(buf), ini_path());
        for (const wchar_t* e = buf; e < buf + n && *e; e += wcslen(e) + 1) {
            const wchar_t* eq = wcschr(e, L'=');
            if (!eq) continue;
            char id[256];
            const int len = WideCharToMultiByte(CP_UTF8, 0, e, (int)(eq - e), id, sizeof(id) - 1, nullptr, nullptr);
            id[len > 0 ? len : 0] = 0;
            for (u32 i = 0; i < g_bench_count; ++i)
                if (!strcmp(g_benches[i].cli_id, id)) selected[i] = eq[1] != L'0';
        }
    }
    s_file.params = *p;
    memcpy(s_file.selected, selected, g_bench_count);
}

static void capture(Snapshot* s, const BenchParams* p, const bool* selected) {
    s->params = *p;
    s->theme = theme_mode();
    s->adv_sensors = adv_enabled();
    memcpy(s->selected, selected, g_bench_count);
}

static bool same(const Snapshot* a, const Snapshot* b) {
    const BenchParams *x = &a->params, *y = &b->params;
    return a->theme == b->theme && a->adv_sensors == b->adv_sensors && x->duration_ms == y->duration_ms && x->thread_count == y->thread_count &&
           x->gpu_adapter == y->gpu_adapter && x->gpu_width == y->gpu_width && x->gpu_height == y->gpu_height &&
           x->gpu_fullscreen == y->gpu_fullscreen && x->gpu_onscreen == y->gpu_onscreen && x->disk_drives == y->disk_drives &&
           x->disk_file_bytes == y->disk_file_bytes && x->disk_passes == y->disk_passes &&
           x->disk_write_through == y->disk_write_through && x->repeat == y->repeat && !memcmp(a->selected, b->selected, g_bench_count);
}

// Appends "key=value\0" to a profile section buffer (double-null terminated by the caller).
struct Section { wchar_t* buf; u32 cap, len; };

static void sec_add(Section* s, const wchar_t* fmt, ...) {
    if (s->len + 2 >= s->cap) return;
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf(s->buf + s->len, s->cap - s->len - 2, fmt, args);
    va_end(args);
    if (n > 0) s->len += (u32)n + 1;
    s->buf[s->len] = 0;
}

static void write_all(const Snapshot* cur) {
    if (!ensure_file()) return;
    Snapshot out = *cur;
    if (s_keep_duration) out.params.duration_ms = s_file.params.duration_ms;
    if (s_keep_theme) out.theme = s_file.theme;
    if (s_keep_selection) memcpy(out.selected, s_file.selected, sizeof(out.selected));
    if (s_keep_present) {
        out.params.gpu_onscreen = s_file.params.gpu_onscreen;
        out.params.gpu_fullscreen = s_file.params.gpu_fullscreen;
    }
    const BenchParams* p = &out.params;

    static wchar_t buf[32768], old[32768];
    Section s = { buf, ARRAY_COUNT(buf), 0 };
    sec_add(&s, L"theme=%s", THEME_NAMES[out.theme < ARRAY_COUNT(THEME_NAMES) ? out.theme : 0]);
    sec_add(&s, L"duration_ms=%u", p->duration_ms);
    sec_add(&s, L"threads=%u", p->thread_count);
    if (p->gpu_adapter < g_sys.gpu_count) {
        wchar_t gpu[256];
        MultiByteToWideChar(CP_UTF8, 0, g_sys.gpus[p->gpu_adapter].name, -1, gpu, ARRAY_COUNT(gpu));
        sec_add(&s, L"gpu=%s", gpu);
    }
    sec_add(&s, L"gpu_width=%u", p->gpu_width);
    sec_add(&s, L"gpu_height=%u", p->gpu_height);
    sec_add(&s, L"gpu_fullscreen=%u", p->gpu_fullscreen ? 1u : 0u);
    sec_add(&s, L"gpu_onscreen=%u", p->gpu_onscreen ? 1u : 0u);
    wchar_t drives[64] = {};
    for (u32 d = 0, n = 0; d < 26; ++d)
        if ((p->disk_drives >> d) & 1) {
            if (n) drives[n++] = L',';
            drives[n++] = (wchar_t)(L'A' + d);
        }
    sec_add(&s, L"disk_drives=%s", drives);
    sec_add(&s, L"disk_file_mib=%llu", p->disk_file_bytes / MiB);
    sec_add(&s, L"disk_passes=%u", p->disk_passes);
    sec_add(&s, L"disk_write_through=%u", p->disk_write_through ? 1u : 0u);
    sec_add(&s, L"repeat=%u", p->repeat);
    sec_add(&s, L"adv_sensors=%u", out.adv_sensors ? 1u : 0u);
    buf[s.len] = 0;
    WritePrivateProfileSectionW(L"settings", buf, s_path);

    // [tests]: every registered id, plus entries from other builds we don't know (kept as they are).
    const DWORD n = GetPrivateProfileSectionW(L"tests", old, ARRAY_COUNT(old), s_path);
    s = { buf, ARRAY_COUNT(buf), 0 };
    wchar_t id[128];
    for (u32 i = 0; i < g_bench_count; ++i) {
        MultiByteToWideChar(CP_UTF8, 0, g_benches[i].cli_id, -1, id, ARRAY_COUNT(id));
        sec_add(&s, L"%s=%u", id, out.selected[i] ? 1u : 0u);
    }
    for (const wchar_t* e = old; e < old + n && *e; e += wcslen(e) + 1) {
        const wchar_t* eq = wcschr(e, L'=');
        if (!eq) continue;
        char id8[256];
        const int len = WideCharToMultiByte(CP_UTF8, 0, e, (int)(eq - e), id8, sizeof(id8) - 1, nullptr, nullptr);
        id8[len > 0 ? len : 0] = 0;
        bool known = false;
        for (u32 i = 0; i < g_bench_count && !known; ++i) known = !strcmp(g_benches[i].cli_id, id8);
        if (!known) sec_add(&s, L"%s", e);
    }
    buf[s.len] = 0;
    WritePrivateProfileSectionW(L"tests", buf, s_path);
    s_file = out;
}

void settings_tick(const BenchParams* p, const bool* selected) {
    Snapshot cur;
    capture(&cur, p, selected);
    if (!s_started) {   // first frame: everything the command line changed differs from the file now
        s_started = true;
        s_last = cur;
        s_start_duration = cur.params.duration_ms;
        s_start_theme = cur.theme;
        memcpy(s_start_selected, cur.selected, sizeof(s_start_selected));
        s_keep_duration = s_start_duration != s_file.params.duration_ms;
        s_keep_theme = s_start_theme != s_file.theme;
        s_keep_selection = memcmp(s_start_selected, s_file.selected, g_bench_count) != 0;
        s_start_onscreen = cur.params.gpu_onscreen;
        s_start_fullscreen = cur.params.gpu_fullscreen;
        s_keep_present = s_start_onscreen != s_file.params.gpu_onscreen || s_start_fullscreen != s_file.params.gpu_fullscreen;
        return;
    }
    if (s_keep_duration && cur.params.duration_ms != s_start_duration) s_keep_duration = false;
    if (s_keep_theme && cur.theme != s_start_theme) s_keep_theme = false;
    if (s_keep_selection && memcmp(cur.selected, s_start_selected, g_bench_count) != 0) s_keep_selection = false;
    if (s_keep_present && (cur.params.gpu_onscreen != s_start_onscreen || cur.params.gpu_fullscreen != s_start_fullscreen))
        s_keep_present = false;
    const f64 now = timer_now();
    if (!same(&cur, &s_last)) {
        s_last = cur;
        s_dirty = true;
        s_dirty_since = now;
    }
    if (s_dirty && now - s_dirty_since >= 1.0) {
        write_all(&cur);
        s_dirty = false;
    }
}

void settings_flush(const BenchParams* p, const bool* selected) {
    if (!s_started) return;
    settings_tick(p, selected);
    if (!s_dirty) return;
    Snapshot cur;
    capture(&cur, p, selected);
    write_all(&cur);
    s_dirty = false;
}
