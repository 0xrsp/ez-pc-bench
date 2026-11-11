// Disk health persistence (see health_store.h).
#include "core/health_store.h"
#include "core/toon.h"
#include <windows.h>
#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

const char* const HEALTH_FIELD_IDS[HF_COUNT] = { "temp_c", "life_used_pct", "written_gb", "read_gb", "power_on_h",
                                                 "reallocated", "pending", "uncorrectable", "media_errors" };
const char* const HEALTH_FIELD_NAMES[HF_COUNT] = { "Temperature", "Life used", "Data written", "Data read", "Power-on time",
                                                   "Reallocated sectors", "Pending sectors", "Uncorrectable sectors",
                                                   "Media errors" };
const char* const HEALTH_FIELD_UNITS[HF_COUNT] = { "\xC2\xB0\x43", "%", "GB", "GB", "h", "", "", "", "" };     // \x43 = 'C'

static wchar_t s_dir[MAX_PATH], s_ini[MAX_PATH];

const wchar_t* health_store_dir() {
    if (!s_dir[0]) {
        PWSTR appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
            swprintf(s_dir, MAX_PATH, L"%s\\ez-pc-bench\\health", appdata);
            swprintf(s_ini, MAX_PATH, L"%s\\health.ini", s_dir);
        }
        CoTaskMemFree(appdata);
    }
    return s_dir;
}

static bool ensure_dir() {
    if (!health_store_dir()[0]) return false;
    wchar_t parent[MAX_PATH];
    wcscpy_s(parent, s_dir);
    if (wchar_t* slash = wcsrchr(parent, L'\\')) *slash = 0;
    CreateDirectoryW(parent, nullptr);
    CreateDirectoryW(s_dir, nullptr);
    return GetFileAttributesW(s_dir) != INVALID_FILE_ATTRIBUTES;
}

// Creates health.ini with a UTF-16 BOM so the profile API writes Unicode.
static const wchar_t* ini_path(bool create) {
    if (!health_store_dir()[0]) return nullptr;
    if (create && ensure_dir()) {
        HANDLE f = CreateFileW(s_ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const u16 bom = 0xFEFF;
            DWORD written;
            WriteFile(f, &bom, sizeof(bom), &written, nullptr);
            CloseHandle(f);
        }
    }
    return s_ini;
}

void health_drive_key(const char* model, const char* serial, char* out, u32 size) {
    u32 h = 2166136261u;     // FNV-1a of "model|serial"
    for (const char* p = model; *p; ++p) h = (h ^ (u8)*p) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (const char* p = serial; *p; ++p) h = (h ^ (u8)*p) * 16777619u;
    char name[49];
    u32 n = 0;
    for (const char* p = model; *p && n < 48; ++p) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (ok) name[n++] = c;
        else if (n && name[n - 1] != '_') name[n++] = '_';
    }
    while (n && name[n - 1] == '_') --n;
    name[n] = 0;
    snprintf(out, size, "%s_%08x", n ? name : "drive", h);
}

static void widen(const char* s, wchar_t* out, u32 count) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)count); }

void health_thresholds_load(const char* key, HealthThresholds* out) {
    *out = HEALTH_THRESHOLDS_DEFAULT;
    const wchar_t* ini = ini_path(false);
    if (!ini) return;
    wchar_t sec[128];
    widen(key, sec, ARRAY_COUNT(sec));
    out->reallocated = GetPrivateProfileIntW(sec, L"reallocated", (INT)out->reallocated, ini);
    out->pending = GetPrivateProfileIntW(sec, L"pending", (INT)out->pending, ini);
    out->uncorrectable = GetPrivateProfileIntW(sec, L"uncorrectable", (INT)out->uncorrectable, ini);
    out->media_errors = GetPrivateProfileIntW(sec, L"media_errors", (INT)out->media_errors, ini);
    out->life_left_pct = GetPrivateProfileIntW(sec, L"life_left_pct", (INT)out->life_left_pct, ini);
    if (out->life_left_pct > 100) out->life_left_pct = 100;
}

void health_thresholds_save(const char* key, const HealthThresholds* t) {
    const wchar_t* ini = ini_path(true);
    if (!ini) return;
    wchar_t sec[128], buf[256];
    widen(key, sec, ARRAY_COUNT(sec));
    const int n = swprintf(buf, ARRAY_COUNT(buf) - 1, L"reallocated=%u%cpending=%u%cuncorrectable=%u%cmedia_errors=%u%clife_left_pct=%u%c",
                           t->reallocated, 0, t->pending, 0, t->uncorrectable, 0, t->media_errors, 0, t->life_left_pct, 0);
    if (n > 0) buf[n] = 0;      // section strings end with an extra null
    WritePrivateProfileSectionW(sec, buf, ini);
}

bool health_ui_option(const char* name, bool def) {
    const wchar_t* ini = ini_path(false);
    if (!ini) return def;
    wchar_t key[64];
    widen(name, key, ARRAY_COUNT(key));
    return GetPrivateProfileIntW(L"ui", key, def ? 1 : 0, ini) != 0;
}

void health_set_ui_option(const char* name, bool value) {
    const wchar_t* ini = ini_path(true);
    if (!ini) return;
    wchar_t key[64];
    widen(name, key, ARRAY_COUNT(key));
    WritePrivateProfileStringW(L"ui", key, value ? L"1" : L"0", ini);
}

// ---- history ---------------------------------------------------------------------------------------

static void history_path(const char* key, wchar_t* out, u32 count) {
    wchar_t k[128];
    widen(key, k, ARRAY_COUNT(k));
    swprintf(out, count, L"%s\\%s.toon.txt", health_store_dir(), k);
}

u32 health_history_load(const char* key, HealthSample* out, u32 max) {
    if (!health_store_dir()[0]) return 0;
    wchar_t path[MAX_PATH];
    history_path(key, path, MAX_PATH);
    ToonReader r;
    char err[128];
    char* buf = toon_read_file(path, &r, err, sizeof(err));
    if (!buf) return 0;
    u32 n = 0;
    i32 col[HF_COUNT + 1];      // field index -> column; [HF_COUNT] = time
    bool in_table = false;
    const char *s, *e;
    u32 ind;
    static char cells[TOON_MAX_FIELDS][TOON_CELL];
    while (toon_next_line(&r, &s, &e, &ind)) {
        if (in_table && ind > 0) {
            const u32 c = toon_split(s, e, cells, TOON_MAX_FIELDS);
            HealthSample hs;
            hs.time = col[HF_COUNT] >= 0 && (u32)col[HF_COUNT] < c ? _atoi64(cells[col[HF_COUNT]]) : 0;
            for (u32 f = 0; f < HF_COUNT; ++f) {
                const char* v = col[f] >= 0 && (u32)col[f] < c ? cells[col[f]] : "null";
                hs.v[f] = !strcmp(v, "null") || !v[0] ? NAN : atof(v);
            }
            if (!hs.time) continue;
            if (n == max) {     // keep the newest `max`
                memmove(out, out + 1, (max - 1) * sizeof(*out));
                --n;
            }
            out[n++] = hs;
            continue;
        }
        in_table = false;
        ToonLine l;
        if (ind != 0 || !toon_parse_key(s, e, &l) || strcmp(l.key, "samples") != 0 || !l.field_count) continue;
        for (u32 f = 0; f <= HF_COUNT; ++f) col[f] = -1;
        for (u32 c = 0; c < l.field_count; ++c) {
            if (!strcmp(l.fields[c], "time")) col[HF_COUNT] = (i32)c;
            for (u32 f = 0; f < HF_COUNT; ++f)
                if (!strcmp(l.fields[c], HEALTH_FIELD_IDS[f])) col[f] = (i32)c;
        }
        in_table = true;
    }
    toon_free_file(buf);
    return n;
}

static bool changed(f64 a, f64 b) { return isfinite(a) != isfinite(b) || (isfinite(a) && a != b); }

bool health_history_add(const char* key, const char* model, const HealthSample* s) {
    static HealthSample hist[HEALTH_HISTORY_MAX];
    u32 n = health_history_load(key, hist, HEALTH_HISTORY_MAX);
    if (n) {
        const HealthSample* last = &hist[n - 1];
        bool wear = false;
        for (u32 f = HF_LIFE_USED; f < HF_COUNT; ++f)
            if (f != HF_WRITTEN_GB && f != HF_READ_GB && f != HF_POWER_ON_H) wear |= changed(s->v[f], last->v[f]);
        if (s->time - last->time < 3600 && !wear) return false;
    }
    if (n == HEALTH_HISTORY_MAX) {
        memmove(hist, hist + 1, (n - 1) * sizeof(*hist));
        --n;
    }
    hist[n++] = *s;
    if (!ensure_dir()) return false;

    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    history_path(key, path, MAX_PATH);
    swprintf(tmp, MAX_PATH, L"%s.tmp", path);
    FILE* f = _wfopen(tmp, L"wb");
    if (!f) return false;
    toon_key(f, 0, "drive");
    toon_str(f, 1, "model", model);
    toon_str(f, 1, "key", key);
    toon_num(f, 1, "version", 1);
    char fields[256] = "time";
    for (u32 k = 0; k < HF_COUNT; ++k) {
        strcat_s(fields, ",");
        strcat_s(fields, HEALTH_FIELD_IDS[k]);
    }
    toon_table(f, 0, "samples", n, fields);
    for (u32 i = 0; i < n; ++i) {
        ToonRow row = toon_row(f, 1);
        toon_cell_num(&row, (f64)hist[i].time);
        for (u32 k = 0; k < HF_COUNT; ++k) toon_cell_num(&row, hist[i].v[k]);
        toon_row_end(&row);
    }
    const bool ok = fclose(f) == 0;
    return ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

bool health_project(const HealthSample* s, u32 n, HealthProjection* out) {
    out->days = 0;
    out->written_gb_per_day = out->life_pct_per_day = NAN;
    out->life_end_time = 0;
    if (n < 2) return false;
    out->days = (s[n - 1].time - s[0].time) / 86400.0;
    if (out->days < 1.0) return false;
    // First and last sample that have the value.
    auto rate = [&](HealthField f, f64* last) {
        i32 a = -1, b = -1;
        for (u32 i = 0; i < n; ++i)
            if (isfinite(s[i].v[f])) { if (a < 0) a = (i32)i; b = (i32)i; }
        if (a < 0 || a == b) return (f64)NAN;
        const f64 days = (s[b].time - s[a].time) / 86400.0;
        *last = s[b].v[f];
        return days >= 1.0 ? (s[b].v[f] - s[a].v[f]) / days : (f64)NAN;
    };
    f64 last = 0.0;
    out->written_gb_per_day = rate(HF_WRITTEN_GB, &last);
    const f64 life = rate(HF_LIFE_USED, &last);
    if (isfinite(life) && life > 0.0) {
        out->life_pct_per_day = life;
        if (last < 100.0) out->life_end_time = s[n - 1].time + (i64)((100.0 - last) / life * 86400.0);
    }
    return true;
}
