// Baseline loading: reads the metrics table of one of our TOON result files (results_write_toon) into a
// hash table keyed by (test id, metric, x).
#include "core/baseline.h"
#include "core/results.h"
#include "core/toon.h"
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
    static const char* const lower[] = { "ns", "\xC2\xB5s", "\xCE\xBCs", "us", "ms", "s" };   // micro sign, Greek mu
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


// ---- TOON loader ---------------------------------------------------------------------------------

static i32 field_index(const ToonLine* l, const char* name) {
    for (u32 i = 0; i < l->field_count; ++i)
        if (!strcmp(l->fields[i], name)) return (i32)i;
    return -1;
}

static bool parse_file(ToonReader* r, char* err, u32 err_size) {
    char app[32] = {};
    bool have_metrics = false;
    const char *s, *e;
    u32 indent;
    while (toon_next_line(r, &s, &e, &indent)) {
        if (indent != 0) continue;      // nested lines and rows of tables we don't need
        ToonLine l;
        if (!toon_parse_key(s, e, &l)) { snprintf(err, err_size, "line %u: expected 'key:'", r->line); return false; }
        if (!strcmp(l.key, "app")) {
            toon_decode(l.value, l.value_end, app, sizeof(app));
        } else if (!strcmp(l.key, "timestamp")) {
            toon_decode(l.value, l.value_end, g_baseline.timestamp, sizeof(g_baseline.timestamp));
        } else if (!strcmp(l.key, "metrics") && l.array) {
            const i32 fid = field_index(&l, "id"), fname = field_index(&l, "name");
            const i32 fvalue = field_index(&l, "value"), fx = field_index(&l, "x");
            if (fid < 0 || fname < 0 || fvalue < 0) { snprintf(err, err_size, "line %u: metrics table lacks id/name/value", r->line); return false; }
            static char cells[TOON_MAX_FIELDS][TOON_CELL];
            for (u32 row = 0; row < l.count; ++row) {
                if (!toon_next_line(r, &s, &e, &indent) || indent == 0) {
                    snprintf(err, err_size, "metrics table ends after %u of %u rows", row, l.count);
                    return false;
                }
                const u32 n = toon_split(s, e, cells, TOON_MAX_FIELDS);
                if (n != l.field_count) { snprintf(err, err_size, "line %u: %u cells, expected %u", r->line, n, l.field_count); return false; }
                char* stop = nullptr;
                const f64 value = strtod(cells[fvalue], &stop);
                if (stop == cells[fvalue] || !isfinite(value)) continue;    // null: no baseline for this metric
                const f64 x = fx >= 0 ? strtod(cells[fx], nullptr) : 0.0;
                if (g_baseline.count >= BASE_MAX) { s_full = true; break; }
                const u32 e_index = g_baseline.count++;
                s_entries[e_index] = { pool_add(cells[fid]), pool_add(cells[fname]), x, value };
                const BaseEntry* b = &s_entries[e_index];
                const u32 slot = find_slot(s_pool + b->id, s_pool + b->name, b->x);
                if (!s_slots[slot]) s_slots[slot] = e_index + 1;     // duplicates: first one wins
            }
            have_metrics = true;
        }
    }
    if (strcmp(app, "ez-pc-bench") != 0 || !have_metrics) { snprintf(err, err_size, "not an EzPcBench results file"); return false; }
    if (s_full) { snprintf(err, err_size, "too many results"); return false; }
    return true;
}

bool baseline_load(const wchar_t* path, char* err, u32 err_size) {
    baseline_clear();
    ToonReader r;
    char* buf = toon_read_file(path, &r, err, err_size);
    if (!buf) return false;
    const bool ok = parse_file(&r, err, err_size);
    toon_free_file(buf);
    if (!ok) { baseline_clear(); return false; }

    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, g_baseline.file, sizeof(g_baseline.file), nullptr, nullptr);
    g_baseline.loaded = true;
    return true;
}
