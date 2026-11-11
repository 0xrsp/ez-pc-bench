// Baseline loading: a small JSON reader for exactly what results_write_json emits (objects, arrays,
// strings with escapes, numbers, true/false/null), plus a hash table keyed by (test id, metric, x).
#include "core/baseline.h"
#include "core/results.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Baseline g_baseline;

constexpr u32 BASE_MAX = 16384;             // metrics
constexpr u32 BASE_SLOTS = 32768;           // hash slots (power of two, >= 2 * BASE_MAX)
constexpr u32 BASE_POOL = 1024 * 1024;      // string bytes

struct BaseEntry {
    u32 id, name;       // offsets into s_pool
    f64 x, value;
};
static BaseEntry s_entries[BASE_MAX];
static u32 s_slots[BASE_SLOTS];             // entry index + 1, 0 = empty
static char s_pool[BASE_POOL];
static u32 s_pool_len;
static bool s_full;                         // a table ran out of space during the load

void baseline_clear() {
    memset(&g_baseline, 0, sizeof(g_baseline));
    memset(s_slots, 0, sizeof(s_slots));
    s_pool_len = 1;     // offset 0 = ""
    s_pool[0] = 0;
    s_full = false;
}

static u32 pool_add(const char* s) {
    const u32 n = (u32)strlen(s) + 1;
    if (s_pool_len + n > BASE_POOL) { s_full = true; return 0; }
    memcpy(s_pool + s_pool_len, s, n);
    s_pool_len += n;
    return s_pool_len - n;
}

static u64 key_hash(const char* id, const char* name, f64 x) {
    u64 h = 0xcbf29ce484222325ull;
    for (const char* p = id; *p; ++p) h = (h ^ (u8)*p) * 0x100000001b3ull;
    h = (h ^ 0xff) * 0x100000001b3ull;
    for (const char* p = name; *p; ++p) h = (h ^ (u8)*p) * 0x100000001b3ull;
    u64 xb;
    memcpy(&xb, &x, sizeof(xb));
    return (h ^ xb) * 0x100000001b3ull;
}

// Returns the slot holding the key, or the empty slot where it would go.
static u32 find_slot(const char* id, const char* name, f64 x) {
    u32 s = (u32)key_hash(id, name, x) & (BASE_SLOTS - 1);
    for (;;) {
        const u32 e = s_slots[s];
        if (!e) return s;
        const BaseEntry* b = &s_entries[e - 1];
        if (b->x == x && !strcmp(s_pool + b->id, id) && !strcmp(s_pool + b->name, name)) return s;
        s = (s + 1) & (BASE_SLOTS - 1);
    }
}

bool baseline_find(const char* test_id, const char* metric, f64 key_x, f64* value) {
    if (!g_baseline.loaded) return false;
    const u32 e = s_slots[find_slot(test_id, metric, key_x)];
    if (!e) return false;
    *value = s_entries[e - 1].value;
    return true;
}

f64 baseline_key_x(const BenchResult* r, u32 k) {
    return r->kind == RESULT_SERIES ? res_metrics(r)[k].x : r->kind == RESULT_MATRIX ? (f64)k : 0.0;
}

bool baseline_metric(const char* test_id, const BenchResult* r, u32 k, f64* value) {
    return baseline_find(test_id, res_metrics(r)[k].name, baseline_key_x(r, k), value);
}

bool baseline_matrix(const char* test_id, const BenchResult* r, f64* lo, f64* avg, f64* hi) {
    const Metric* m = res_metrics(r);
    f64 sum = 0.0;
    u32 n = 0;
    for (u32 k = 0; k < r->count; ++k) {
        if (r->rows == r->cols && r->cols && k / r->cols == k % r->cols) continue;     // diagonal
        f64 v;
        if (!baseline_find(test_id, m[k].name, (f64)k, &v)) continue;
        *lo = n == 0 || v < *lo ? v : *lo;
        *hi = n == 0 || v > *hi ? v : *hi;
        sum += v;
        ++n;
    }
    if (n) *avg = sum / n;
    return n > 0;
}

bool unit_lower_is_better(const char* unit) {
    static const char* const lower[] = { "ns", "\xC2\xB5s", "\xCE\xBCs", "us", "ms", "s" };   // µ: micro sign, Greek mu
    for (u32 i = 0; i < ARRAY_COUNT(lower); ++i)
        if (unit && !strcmp(unit, lower[i])) return true;
    return false;
}

f64 delta_pct(f64 value, f64 base) {
    if (base == 0.0) return value == 0.0 ? 0.0 : (value > 0.0 ? HUGE_VAL : -HUGE_VAL);
    return (value - base) / fabs(base) * 100.0;
}

i32 delta_rating(f64 pct, const char* unit) {
    if (!(fabs(pct) >= 1.0)) return 0;      // also NaN
    const bool up = pct > 0.0;
    return up != unit_lower_is_better(unit) ? 1 : -1;
}

void format_delta(char* buf, u32 size, f64 pct) {
    if (!isfinite(pct)) snprintf(buf, size, pct > 0.0 ? "+inf" : pct < 0.0 ? "-inf" : "n/a");
    else if (fabs(pct) < 0.05) snprintf(buf, size, "0.0%%");
    else snprintf(buf, size, fabs(pct) >= 100.0 ? "%+.0f%%" : "%+.1f%%", pct);
}

// ---- JSON reader -----------------------------------------------------------------------

struct Json {
    const char* begin;
    const char* p;
    const char* end;
    bool err;
    u32 depth;
};

static void ws(Json* j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) ++j->p;
}

static bool expect(Json* j, char c) {
    ws(j);
    if (j->p < j->end && *j->p == c) { ++j->p; return true; }
    j->err = true;
    return false;
}

static u32 hex4(const char* p) {
    u32 v = 0;
    for (u32 i = 0; i < 4; ++i) {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
        else return 0xFFFFFFFFu;
    }
    return v;
}

// Reads a string into out (UTF-8, truncated to size - 1).
static bool jstr(Json* j, char* out, u32 size) {
    if (!expect(j, '"')) return false;
    u32 n = 0;
    auto put = [&](char c) { if (n + 1 < size) out[n++] = c; };
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { put(c); continue; }
        if (j->p >= j->end) break;
        c = *j->p++;
        switch (c) {
            case 'n': put('\n'); break;
            case 't': put('\t'); break;
            case 'r': put('\r'); break;
            case 'b': put('\b'); break;
            case 'f': put('\f'); break;
            case 'u': {
                if (j->end - j->p < 4) { j->err = true; return false; }
                u32 cp = hex4(j->p);
                j->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    const u32 lo = hex4(j->p + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; }
                }
                if (cp == 0xFFFFFFFFu) { j->err = true; return false; }
                if (cp < 0x80) put((char)cp);
                else if (cp < 0x800) { put((char)(0xC0 | (cp >> 6))); put((char)(0x80 | (cp & 0x3F))); }
                else if (cp < 0x10000) {
                    put((char)(0xE0 | (cp >> 12))); put((char)(0x80 | ((cp >> 6) & 0x3F))); put((char)(0x80 | (cp & 0x3F)));
                } else {
                    put((char)(0xF0 | (cp >> 18))); put((char)(0x80 | ((cp >> 12) & 0x3F)));
                    put((char)(0x80 | ((cp >> 6) & 0x3F))); put((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: put(c); break;     // \" \\ \/
        }
    }
    if (size) out[n] = 0;
    if (j->p >= j->end) { j->err = true; return false; }
    ++j->p;     // closing quote
    return true;
}

static bool word(Json* j, const char* w) {
    const size_t n = strlen(w);
    if ((size_t)(j->end - j->p) >= n && !memcmp(j->p, w, n)) { j->p += n; return true; }
    return false;
}

// Number, or null (-> NaN).
static bool jnum(Json* j, f64* out) {
    ws(j);
    if (word(j, "null")) { *out = NAN; return true; }
    char buf[64];
    u32 n = 0;
    while (j->p < j->end && n + 1 < sizeof(buf) && strchr("+-0123456789.eE", *j->p) && *j->p) buf[n++] = *j->p++;
    buf[n] = 0;
    char* stop = nullptr;
    *out = strtod(buf, &stop);
    if (n == 0 || *stop) { j->err = true; return false; }
    return true;
}

static bool jskip(Json* j);

// Object members: call after '{'. True with the key read and ':' consumed; false at '}' or on error.
static bool obj_next(Json* j, bool* first, char* key, u32 size) {
    ws(j);
    if (j->err || j->p >= j->end) { j->err = true; return false; }
    if (*j->p == '}') { ++j->p; return false; }
    if (!*first && !expect(j, ',')) return false;
    *first = false;
    return jstr(j, key, size) && expect(j, ':');
}

// Array elements: call after '['. True when another value follows; false at ']' or on error.
static bool arr_next(Json* j, bool* first) {
    ws(j);
    if (j->err || j->p >= j->end) { j->err = true; return false; }
    if (*j->p == ']') { ++j->p; return false; }
    if (!*first && !expect(j, ',')) return false;
    *first = false;
    return true;
}

static bool jskip(Json* j) {
    ws(j);
    if (j->p >= j->end || ++j->depth > 64) { j->err = true; return false; }
    bool first = true;
    char key[8];
    switch (*j->p) {
        case '{':
            ++j->p;
            while (obj_next(j, &first, key, sizeof(key))) jskip(j);
            break;
        case '[':
            ++j->p;
            while (arr_next(j, &first)) jskip(j);
            break;
        case '"': {
            char tmp[4];
            jstr(j, tmp, sizeof(tmp));
            break;
        }
        default:
            if (!word(j, "true") && !word(j, "false")) {
                f64 v;
                jnum(j, &v);
            }
    }
    --j->depth;
    return !j->err;
}

static void add_entry(u32 name, f64 x, f64 value) {
    if (g_baseline.count >= BASE_MAX) { s_full = true; return; }
    s_entries[g_baseline.count++] = { 0, name, x, value };
}

// One element of "results": { "id", ..., "kind", "metrics": [ { "name", "unit", "value", "x"? } ] }.
static void parse_result(Json* j) {
    if (!expect(j, '{')) return;
    char key[32], id[128] = {}, kind[16] = {};
    const u32 first_entry = g_baseline.count;
    bool first = true;
    while (obj_next(j, &first, key, sizeof(key))) {
        if (!strcmp(key, "id")) jstr(j, id, sizeof(id));
        else if (!strcmp(key, "kind")) jstr(j, kind, sizeof(kind));
        else if (!strcmp(key, "metrics")) {
            if (!expect(j, '[')) return;
            bool first_m = true;
            for (u32 index = 0; arr_next(j, &first_m); ++index) {
                if (!expect(j, '{')) return;
                char name[128] = {};
                f64 value = NAN, x = (f64)index;    // no "x" (values, matrix): position in the list
                bool first_f = true;
                while (obj_next(j, &first_f, key, sizeof(key))) {
                    if (!strcmp(key, "name")) jstr(j, name, sizeof(name));
                    else if (!strcmp(key, "value")) jnum(j, &value);
                    else if (!strcmp(key, "x")) jnum(j, &x);
                    else jskip(j);
                }
                if (j->err) return;
                if (isfinite(value)) add_entry(pool_add(name), x, value);
            }
        } else {
            jskip(j);
        }
    }
    if (j->err) return;
    // Key order in the object is free: fix up id and plain-value keys now that everything is known.
    const u32 id_off = pool_add(id);
    const bool values = strcmp(kind, "series") != 0 && strcmp(kind, "matrix") != 0;
    for (u32 e = first_entry; e < g_baseline.count; ++e) {
        BaseEntry* b = &s_entries[e];
        b->id = id_off;
        if (values) b->x = 0.0;
        const u32 slot = find_slot(s_pool + b->id, s_pool + b->name, b->x);
        if (!s_slots[slot]) s_slots[slot] = e + 1;     // duplicates: first one wins
    }
}

static bool parse_file(Json* j, char* err, u32 err_size) {
    if (!expect(j, '{')) { snprintf(err, err_size, "not a JSON object"); return false; }
    char key[32], app[32] = {};
    bool first = true, have_results = false;
    while (obj_next(j, &first, key, sizeof(key))) {
        if (!strcmp(key, "app")) jstr(j, app, sizeof(app));
        else if (!strcmp(key, "timestamp")) jstr(j, g_baseline.timestamp, sizeof(g_baseline.timestamp));
        else if (!strcmp(key, "results")) {
            have_results = true;
            if (!expect(j, '[')) break;
            bool first_r = true;
            while (arr_next(j, &first_r)) parse_result(j);
        } else {
            jskip(j);
        }
    }
    if (j->err) { snprintf(err, err_size, "malformed JSON near byte %u", (u32)(j->p - j->begin)); return false; }
    if (strcmp(app, "ez-pc-bench") != 0 || !have_results) { snprintf(err, err_size, "not an ez-pc-bench results file"); return false; }
    if (s_full) { snprintf(err, err_size, "too many results"); return false; }
    return true;
}

bool baseline_load(const wchar_t* path, char* err, u32 err_size) {
    baseline_clear();
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { snprintf(err, err_size, "cannot open file (error %lu)", GetLastError()); return false; }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 64 * (i64)MiB) {
        CloseHandle(f);
        snprintf(err, err_size, "unexpected file size");
        return false;
    }
    char* buf = (char*)VirtualAlloc(nullptr, (SIZE_T)size.QuadPart + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    const bool read = buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart;
    CloseHandle(f);
    if (!read) {
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        snprintf(err, err_size, "read failed");
        return false;
    }
    Json j = { buf, buf, buf + got, false, 0 };
    if (got >= 3 && !memcmp(buf, "\xEF\xBB\xBF", 3)) j.p += 3;     // tolerate a UTF-8 BOM
    const bool ok = parse_file(&j, err, err_size);
    VirtualFree(buf, 0, MEM_RELEASE);
    if (!ok) { baseline_clear(); return false; }

    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, g_baseline.file, sizeof(g_baseline.file), nullptr, nullptr);
    g_baseline.loaded = true;
    return true;
}
