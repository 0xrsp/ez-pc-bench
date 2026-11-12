// Branchy "developer" workloads: a validating JSON parser and a word tokenizer / counter. Both inputs are
// generated deterministically with their expected statistics, and every timed pass checks them.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/throughput.h"
#include <string.h>

static u64 rng(u64* s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

// ---- JSON ----------------------------------------------------------------------------------------------

static constexpr u64 JSON_CAP = 1 * MiB;

struct JsonStats {
    u64 objects, arrays, strings, numbers, literals;
    u64 str_bytes, str_sum;     // decoded string bytes and the sum of their values
    f64 num_sum;                // every number is a multiple of 1/8 below 2^40: sums are exact
};

struct JsonDoc {
    u8* text;
    u64 size;
    JsonStats expect;
};
static JsonDoc s_json;

// Writer that tracks what a correct parser must see.
struct JsonGen {
    u8* p;
    u8* end;
    JsonStats* st;
    u64 seed;
};

static void jw(JsonGen* g, const char* s) { while (*s && g->p < g->end) *g->p++ = (u8)*s++; }
static void jw_byte(JsonGen* g, u8 c) { if (g->p < g->end) *g->p++ = c; }

static void jw_uint(JsonGen* g, u64 v) {
    char tmp[24];
    u32 n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) jw_byte(g, (u8)tmp[--n]);
}

static const char* const KEYS[] = { "id", "name", "email", "tags", "score", "active", "parent", "location", "items",
                                    "price", "quantity", "description", "created", "ratio", "flags" };
static const char* const VALS[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel",
                                    "india", "juliet", "kilo", "lima", "mike", "november", "oscar", "papa" };

// Plain string (no escapes) or one with escapes; counts the decoded bytes.
static void jw_string(JsonGen* g, const char* s, bool escapes) {
    jw_byte(g, '"');
    for (const char* c = s; *c; ++c) { jw_byte(g, (u8)*c); g->st->str_bytes++; g->st->str_sum += (u8)*c; }
    if (escapes) {
        // \" \\ \n \t é -> " \ LF TAB and U+00E9 as UTF-8 (C3 A9)
        jw(g, " \\\"q\\\" \\\\ \\n\\t caf\\u00e9");
        static const u8 decoded[] = { ' ', '"', 'q', '"', ' ', '\\', ' ', '\n', '\t', ' ', 'c', 'a', 'f', 0xC3, 0xA9 };
        for (u8 d : decoded) { g->st->str_bytes++; g->st->str_sum += d; }
    }
    jw_byte(g, '"');
    g->st->strings++;
}

// Number: integer, decimal with 1-3 fraction digits of a multiple of 1/8, or with an exponent.
static void jw_number(JsonGen* g) {
    const u64 r = rng(&g->seed);
    const bool neg = (r & 3) == 0;
    u64 whole = (r >> 8) % 100000;
    if (neg) jw_byte(g, '-');
    f64 v;
    switch ((r >> 4) % 3) {
        case 0: jw_uint(g, whole); v = (f64)whole; break;
        case 1: {
            static const char* const FR[] = { "", ".125", ".25", ".375", ".5", ".625", ".75", ".875" };
            const u32 f = (u32)((r >> 32) % 8);
            jw_uint(g, whole);
            jw(g, FR[f]);
            v = whole + f / 8.0;
            break;
        }
        default:
            whole %= 1000;
            jw_uint(g, whole);
            jw(g, "e2");
            v = whole * 100.0;
            break;
    }
    g->st->num_sum += neg ? -v : v;
    g->st->numbers++;
}

static void jw_record(JsonGen* g) {
    jw(g, "{");
    g->st->objects++;
    const u32 fields = 4 + (u32)(rng(&g->seed) % 8);
    for (u32 f = 0; f < fields; ++f) {
        if (f) jw(g, ", ");
        jw_string(g, KEYS[(f * 7 + rng(&g->seed) % 3) % ARRAY_COUNT(KEYS)], false);
        jw(g, ": ");
        const u64 r = rng(&g->seed);
        switch (r % 7) {
            case 0: case 1: jw_number(g); break;
            case 2: jw_string(g, VALS[(r >> 8) % ARRAY_COUNT(VALS)], (r >> 16) % 5 == 0); break;
            case 3: jw(g, (r >> 8) & 1 ? "true" : (r >> 9) & 1 ? "false" : "null"); g->st->literals++; break;
            case 4: {
                jw(g, "[");
                g->st->arrays++;
                const u32 n = (u32)((r >> 8) % 6);
                for (u32 i = 0; i < n; ++i) { if (i) jw(g, ","); jw_number(g); }
                jw(g, "]");
                break;
            }
            case 5: {
                jw(g, "[");
                g->st->arrays++;
                const u32 n = 1 + (u32)((r >> 8) % 4);
                for (u32 i = 0; i < n; ++i) { if (i) jw(g, ", "); jw_string(g, VALS[(r >> (12 + i)) % ARRAY_COUNT(VALS)], false); }
                jw(g, "]");
                break;
            }
            default: {
                jw(g, "{\"lat\": ");
                g->st->objects++;
                g->st->strings++;
                for (const char* c = "lat"; *c; ++c) { g->st->str_bytes++; g->st->str_sum += (u8)*c; }
                jw_number(g);
                jw(g, ", \"lon\": ");
                g->st->strings++;
                for (const char* c = "lon"; *c; ++c) { g->st->str_bytes++; g->st->str_sum += (u8)*c; }
                jw_number(g);
                jw(g, "}");
                break;
            }
        }
    }
    jw(g, "}");
}

static bool json_generate(JsonDoc* d) {
    d->text = (u8*)big_alloc(JSON_CAP);
    if (!d->text) return false;
    memset(&d->expect, 0, sizeof(d->expect));
    JsonGen g = { d->text, d->text + JSON_CAP, &d->expect, 0x5DEECE66Dull };
    jw(&g, "[\n");
    d->expect.arrays++;
    u32 n = 0;
    while (g.end - g.p > 4096) {        // a record is < 1 KiB
        if (n++) jw(&g, ",\n");
        jw_record(&g);
    }
    jw(&g, "\n]\n");
    d->size = (u64)(g.p - d->text);
    return true;
}

// ---- parser

struct JsonParser {
    const u8* p;
    const u8* end;
    JsonStats st;
    u32 depth;
};

static inline void skip_ws(JsonParser* j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) ++j->p;
}

static inline void str_byte(JsonParser* j, u32 c) { j->st.str_bytes++; j->st.str_sum += c; }

static bool parse_string(JsonParser* j) {
    ++j->p;     // opening quote
    for (;;) {
        if (j->p >= j->end) return false;
        u32 c = *j->p++;
        if (c == '"') break;
        if (c < 0x20) return false;
        if (c != '\\') { str_byte(j, c); continue; }
        if (j->p >= j->end) return false;
        c = *j->p++;
        switch (c) {
            case '"': case '\\': case '/': str_byte(j, c); break;
            case 'n': str_byte(j, '\n'); break;
            case 't': str_byte(j, '\t'); break;
            case 'r': str_byte(j, '\r'); break;
            case 'b': str_byte(j, '\b'); break;
            case 'f': str_byte(j, '\f'); break;
            case 'u': {
                if (j->end - j->p < 4) return false;
                u32 cp = 0;
                for (u32 i = 0; i < 4; ++i) {
                    const u32 h = *j->p++;
                    const u32 v = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : 99;
                    if (v > 15) return false;
                    cp = cp << 4 | v;
                }
                if (cp < 0x80) str_byte(j, cp);
                else if (cp < 0x800) { str_byte(j, 0xC0 | cp >> 6); str_byte(j, 0x80 | (cp & 0x3F)); }
                else { str_byte(j, 0xE0 | cp >> 12); str_byte(j, 0x80 | (cp >> 6 & 0x3F)); str_byte(j, 0x80 | (cp & 0x3F)); }
                break;
            }
            default: return false;
        }
    }
    j->st.strings++;
    return true;
}

static const f64 POW10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                             1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

static bool parse_number(JsonParser* j) {
    bool neg = false;
    if (*j->p == '-') { neg = true; ++j->p; }
    u64 mant = 0;
    i32 exp10 = 0, digits = 0;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (digits < 19) { mant = mant * 10 + (*j->p - '0'); ++digits; } else { ++exp10; }
        ++j->p;
    }
    if (j->p < j->end && *j->p == '.') {
        ++j->p;
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            if (digits < 19) { mant = mant * 10 + (*j->p - '0'); ++digits; --exp10; }
            ++j->p;
        }
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        ++j->p;
        bool eneg = false;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) eneg = *j->p++ == '-';
        if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
        i32 e = 0;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') { if (e < 10000) e = e * 10 + (*j->p - '0'); ++j->p; }
        exp10 += eneg ? -e : e;
    }
    f64 v = (f64)mant;
    while (exp10 > 22) { v *= 1e22; exp10 -= 22; }
    while (exp10 < -22) { v /= 1e22; exp10 += 22; }
    v = exp10 >= 0 ? v * POW10[exp10] : v / POW10[-exp10];
    j->st.num_sum += neg ? -v : v;
    j->st.numbers++;
    return true;
}

static bool parse_literal(JsonParser* j, const char* lit, u32 n) {
    if ((u64)(j->end - j->p) < n || memcmp(j->p, lit, n) != 0) return false;
    j->p += n;
    j->st.literals++;
    return true;
}

static bool parse_value(JsonParser* j) {
    skip_ws(j);
    if (j->p >= j->end) return false;
    switch (*j->p) {
        case '{': {
            if (++j->depth > 64) return false;
            ++j->p;
            j->st.objects++;
            skip_ws(j);
            if (j->p < j->end && *j->p == '}') { ++j->p; --j->depth; return true; }
            for (;;) {
                skip_ws(j);
                if (j->p >= j->end || *j->p != '"' || !parse_string(j)) return false;
                skip_ws(j);
                if (j->p >= j->end || *j->p++ != ':') return false;
                if (!parse_value(j)) return false;
                skip_ws(j);
                if (j->p >= j->end) return false;
                const u8 c = *j->p++;
                if (c == '}') break;
                if (c != ',') return false;
            }
            --j->depth;
            return true;
        }
        case '[': {
            if (++j->depth > 64) return false;
            ++j->p;
            j->st.arrays++;
            skip_ws(j);
            if (j->p < j->end && *j->p == ']') { ++j->p; --j->depth; return true; }
            for (;;) {
                if (!parse_value(j)) return false;
                skip_ws(j);
                if (j->p >= j->end) return false;
                const u8 c = *j->p++;
                if (c == ']') break;
                if (c != ',') return false;
            }
            --j->depth;
            return true;
        }
        case '"': return parse_string(j);
        case 't': return parse_literal(j, "true", 4);
        case 'f': return parse_literal(j, "false", 5);
        case 'n': return parse_literal(j, "null", 4);
        default: return parse_number(j);
    }
}

static bool json_parse(const u8* text, u64 size, JsonStats* out) {
    JsonParser j = { text, text + size, {}, 0 };
    const bool ok = parse_value(&j);
    skip_ws(&j);
    *out = j.st;
    return ok && j.p == j.end;
}

static bool json_stats_equal(const JsonStats* a, const JsonStats* b) {
    return a->objects == b->objects && a->arrays == b->arrays && a->strings == b->strings && a->numbers == b->numbers &&
           a->literals == b->literals && a->str_bytes == b->str_bytes && a->str_sum == b->str_sum &&
           a->num_sum == b->num_sum;
}

struct ParseCtx { std::atomic<bool> failed; };

static f64 kern_json(void* ctx, u32, u32) {
    JsonStats st;
    if (!json_parse(s_json.text, s_json.size, &st) || !json_stats_equal(&st, &s_json.expect))
        ((ParseCtx*)ctx)->failed.store(true, std::memory_order_relaxed);
    return (f64)s_json.size;
}

void cpu_parse_json(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!s_json.text && !json_generate(&s_json)) { res_fail(out, "out of memory"); return; }
    ParseCtx c;
    c.failed.store(false);
    st_mt_test(kern_json, &c, "MB/s", 1e6, p, live, out, cancel);
    if (c.failed.load()) res_fail(out, "JSON parse result differs from the generated document");
}

// ---- text tokenizer --------------------------------------------------------------------------------------
// Splits prose into words (letters and digits), lowercases them and counts each distinct word in an
// open-addressing hash table, like the first stage of an indexer or a word-frequency tool.

static constexpr u64 TEXT_CAP = 1 * MiB;
static constexpr u32 VOCAB = 4096;
static constexpr u32 TABLE_BITS = 14;      // 16384 slots, 4x the vocabulary

struct TextDoc {
    u8* text;
    u64 size;
    u64 words;          // expected token count
    u32 distinct;       // expected distinct words
};
static TextDoc s_text;

// Word i: 1, 2 or 3 two-letter syllables from i's base-16 digits, so all words are distinct.
static u32 vocab_word(u32 i, char* out) {
    static const char SYL[16][3] = { "ka", "to", "ri", "ne", "mu", "sa", "lo", "pe", "di", "va", "gu", "ze", "bo", "fi", "ha", "ju" };
    const u32 syl = i < 16 ? 1 : i < 256 ? 2 : 3;
    u32 n = 0;
    for (u32 s = syl; s-- > 0;) { out[n++] = SYL[(i >> (4 * s)) & 15][0]; out[n++] = SYL[(i >> (4 * s)) & 15][1]; }
    return n;
}

static bool text_generate(TextDoc* d) {
    d->text = (u8*)big_alloc(TEXT_CAP);
    if (!d->text) return false;
    static bool used[VOCAB];
    memset(used, 0, sizeof(used));
    u64 s = 0x0123456789ABCDEFull, at = 0;
    d->words = 0;
    bool sentence_start = true;
    while (at + 64 < TEXT_CAP) {
        const u64 r = rng(&s);
        const u32 idx = (u32)((r % VOCAB) * ((r >> 24) % VOCAB) / VOCAB);   // skewed: common words are short
        char w[8];
        const u32 n = vocab_word(idx, w);
        if (sentence_start) w[0] = (char)(w[0] - 32);       // capitalized; the tokenizer lowercases
        memcpy(d->text + at, w, n);
        at += n;
        used[idx] = true;
        d->words++;
        const u32 punct = (u32)((r >> 40) % 16);
        sentence_start = punct == 0;
        const char* sep = punct == 0 ? ". " : punct == 1 ? ", " : punct == 2 ? ";\n" : punct == 3 ? " - " : " ";
        while (*sep) d->text[at++] = (u8)*sep++;
    }
    d->size = at;
    d->distinct = 0;
    for (u32 i = 0; i < VOCAB; ++i) d->distinct += used[i];
    return true;
}

struct WordSlot {
    u64 hash;
    u32 offset;         // first occurrence in the text
    u16 len;
    u16 gen;            // slot is in use when gen == the table's current generation
    u32 count;
    u32 pad;
};

struct TokenCtx {
    WordSlot* tables;   // per thread
    u16 gen[MAX_LP];
    std::atomic<bool> failed;
};

static inline bool is_word_char(u8 c) { return (u8)((c | 0x20) - 'a') < 26 || (u8)(c - '0') < 10; }

static f64 kern_tokenize(void* ctx, u32 index, u32) {
    TokenCtx* c = (TokenCtx*)ctx;
    WordSlot* table = c->tables + ((u64)index << TABLE_BITS);
    u16 gen = ++c->gen[index];
    if (gen == 0) {             // wrapped: clear for real
        memset(table, 0, sizeof(WordSlot) << TABLE_BITS);
        gen = c->gen[index] = 1;
    }
    const u8* t = s_text.text;
    const u64 size = s_text.size;
    u64 words = 0;
    u32 distinct = 0;
    for (u64 i = 0; i < size;) {
        while (i < size && !is_word_char(t[i])) ++i;
        if (i >= size) break;
        const u64 start = i;
        u64 h = 0xcbf29ce484222325ull;
        while (i < size && is_word_char(t[i])) { h = (h ^ (u8)(t[i] | 0x20)) * 0x100000001b3ull; ++i; }
        const u16 len = (u16)(i - start);
        ++words;
        for (u32 slot = (u32)(h >> (64 - TABLE_BITS));; slot = (slot + 1) & ((1u << TABLE_BITS) - 1)) {
            WordSlot* w = &table[slot];
            if (w->gen != gen) {
                *w = { h, (u32)start, len, gen, 1, 0 };
                ++distinct;
                break;
            }
            if (w->hash == h && w->len == len) {
                bool same = true;
                for (u32 k = 0; k < len && same; ++k) same = (t[w->offset + k] | 0x20) == (t[start + k] | 0x20);
                if (same) { w->count++; break; }
            }
        }
    }
    if (words != s_text.words || distinct != s_text.distinct) c->failed.store(true, std::memory_order_relaxed);
    return (f64)size;
}

void cpu_parse_text(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!s_text.text && !text_generate(&s_text)) { res_fail(out, "out of memory"); return; }
    static TokenCtx c;
    const u32 mt = thread_count_for(p->thread_count);
    c.tables = (WordSlot*)big_alloc((u64)mt * (sizeof(WordSlot) << TABLE_BITS));
    if (!c.tables) { res_fail(out, "out of memory"); return; }
    memset(c.gen, 0, sizeof(c.gen));
    c.failed.store(false);
    st_mt_test(kern_tokenize, &c, "MB/s", 1e6, p, live, out, cancel);
    big_free(c.tables);
    if (c.failed.load()) res_fail(out, "word counts differ from the generated text");
}
