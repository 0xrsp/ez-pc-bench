#include "core/json.h"
#include <stdlib.h>
#include <string.h>

namespace {

constexpr u32 MAX_DEPTH = 64;

struct Parser {
    const char* s;
    u32 len, pos, depth;
    JsonDoc* doc;
};

void skip_ws(Parser* p) {
    while (p->pos < p->len) {
        const char c = p->s[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++p->pos;
    }
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

i32 hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

u32 new_node(Parser* p, JsonType type) {
    JsonDoc* d = p->doc;
    if (d->count >= d->cap) return JSON_NONE;
    const u32 i = d->count++;
    d->nodes[i] = { type, p->pos, 0, 0 };
    return i;
}

void close_node(Parser* p, u32 i) {
    JsonNode* n = &p->doc->nodes[i];
    if (n->type != JSON_STRING) n->len = p->pos - n->start;
    n->end = p->doc->count;
}

bool parse_string(Parser* p) {     // at the opening quote
    const u32 i = new_node(p, JSON_STRING);
    if (i == JSON_NONE) return false;
    const u32 start = ++p->pos;
    for (;;) {
        if (p->pos >= p->len) return false;
        const char c = p->s[p->pos];
        if (c == '"') break;
        if ((u8)c < 0x20) return false;
        if (c == '\\') {
            if (++p->pos >= p->len) return false;
            const char e = p->s[p->pos];
            if (e == 'u') {
                if (p->pos + 4 >= p->len) return false;
                for (u32 k = 1; k <= 4; ++k)
                    if (hex_value(p->s[p->pos + k]) < 0) return false;
                p->pos += 4;
            } else if (!strchr("\"\\/bfnrt", e)) {
                return false;
            }
        }
        ++p->pos;
    }
    JsonNode* n = &p->doc->nodes[i];
    n->start = start;
    n->len = p->pos - start;
    ++p->pos;       // closing quote
    close_node(p, i);
    return true;
}

bool parse_number(Parser* p) {
    const u32 i = new_node(p, JSON_NUMBER);
    if (i == JSON_NONE) return false;
    const char* s = p->s;
    if (s[p->pos] == '-') ++p->pos;
    if (p->pos >= p->len || !is_digit(s[p->pos])) return false;
    if (s[p->pos] == '0') ++p->pos;
    else while (p->pos < p->len && is_digit(s[p->pos])) ++p->pos;
    if (p->pos < p->len && s[p->pos] == '.') {
        if (++p->pos >= p->len || !is_digit(s[p->pos])) return false;
        while (p->pos < p->len && is_digit(s[p->pos])) ++p->pos;
    }
    if (p->pos < p->len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
        ++p->pos;
        if (p->pos < p->len && (s[p->pos] == '+' || s[p->pos] == '-')) ++p->pos;
        if (p->pos >= p->len || !is_digit(s[p->pos])) return false;
        while (p->pos < p->len && is_digit(s[p->pos])) ++p->pos;
    }
    close_node(p, i);
    return true;
}

bool parse_literal(Parser* p, const char* word, JsonType type) {
    const u32 n = (u32)strlen(word);
    if (p->len - p->pos < n || memcmp(p->s + p->pos, word, n) != 0) return false;
    const u32 i = new_node(p, type);
    if (i == JSON_NONE) return false;
    p->pos += n;
    close_node(p, i);
    return true;
}

bool parse_value(Parser* p);

// Object or array: `close` is '}' or ']'; objects read "key": value pairs.
bool parse_container(Parser* p, JsonType type, char close) {
    if (++p->depth > MAX_DEPTH) return false;
    const u32 i = new_node(p, type);
    if (i == JSON_NONE) return false;
    ++p->pos;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == close) {
        ++p->pos;
    } else {
        for (;;) {
            skip_ws(p);
            if (type == JSON_OBJECT) {
                if (p->pos >= p->len || p->s[p->pos] != '"' || !parse_string(p)) return false;
                skip_ws(p);
                if (p->pos >= p->len || p->s[p->pos] != ':') return false;
                ++p->pos;
            }
            if (!parse_value(p)) return false;
            skip_ws(p);
            if (p->pos >= p->len) return false;
            const char c = p->s[p->pos++];
            if (c == close) break;
            if (c != ',') return false;
        }
    }
    --p->depth;
    close_node(p, i);
    return true;
}

bool parse_value(Parser* p) {
    skip_ws(p);
    if (p->pos >= p->len) return false;
    switch (p->s[p->pos]) {
        case '{': return parse_container(p, JSON_OBJECT, '}');
        case '[': return parse_container(p, JSON_ARRAY, ']');
        case '"': return parse_string(p);
        case 't': return parse_literal(p, "true", JSON_BOOL);
        case 'f': return parse_literal(p, "false", JSON_BOOL);
        case 'n': return parse_literal(p, "null", JSON_NULL);
        default: return parse_number(p);
    }
}

void put_utf8(char* out, u32 size, u32* o, u32 cp, bool* ok) {
    char b[4];
    u32 n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (*o + n >= size) { *ok = false; return; }
    memcpy(out + *o, b, n);
    *o += n;
}

u32 hex4(const char* s) { return (u32)(hex_value(s[0]) << 12 | hex_value(s[1]) << 8 | hex_value(s[2]) << 4 | hex_value(s[3])); }

}   // namespace

bool json_parse(JsonDoc* doc, const char* text, u32 len, JsonNode* nodes, u32 cap) {
    *doc = { text, nodes, 0, cap };
    Parser p = { text, len, 0, 0, doc };
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) p.pos = 3;   // UTF-8 BOM
    if (!parse_value(&p)) { doc->count = 0; return false; }
    skip_ws(&p);
    while (p.pos < len && text[p.pos] == 0) ++p.pos;    // tolerate a terminating NUL
    if (p.pos != len) { doc->count = 0; return false; }
    return true;
}

u32 json_find(const JsonDoc* doc, u32 obj, const char* key) {
    if (obj >= doc->count || doc->nodes[obj].type != JSON_OBJECT) return JSON_NONE;
    const u32 end = doc->nodes[obj].end;
    char name[128];
    for (u32 k = obj + 1; k + 1 < end; k = doc->nodes[k + 1].end)
        if (json_string(doc, k, name, sizeof(name)) && !strcmp(name, key)) return k + 1;
    return JSON_NONE;
}

u32 json_first(const JsonDoc* doc, u32 node) {
    if (node >= doc->count || doc->nodes[node].type != JSON_ARRAY) return JSON_NONE;
    return node + 1 < doc->nodes[node].end ? node + 1 : JSON_NONE;
}

u32 json_next(const JsonDoc* doc, u32 parent, u32 elem) {
    if (parent >= doc->count || elem >= doc->count) return JSON_NONE;
    const u32 n = doc->nodes[elem].end;
    return n < doc->nodes[parent].end ? n : JSON_NONE;
}

bool json_string(const JsonDoc* doc, u32 node, char* out, u32 size) {
    if (!size) return false;
    out[0] = 0;
    if (node >= doc->count || doc->nodes[node].type != JSON_STRING) return false;
    const char* s = doc->text + doc->nodes[node].start;
    const u32 len = doc->nodes[node].len;
    u32 o = 0;
    bool ok = true;
    for (u32 i = 0; i < len && ok; ++i) {
        char c = s[i];
        if (c != '\\') {
            if (o + 1 >= size) { ok = false; break; }
            out[o++] = c;
            continue;
        }
        c = s[++i];     // the parser validated every escape
        if (c == 'u') {
            u32 cp = hex4(s + i + 1);
            i += 4;
            // Surrogate pair: high followed by \uDC00..DFFF; a lone surrogate becomes U+FFFD.
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < len && s[i + 1] == '\\' && s[i + 2] == 'u') {
                const u32 lo = hex4(s + i + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
            put_utf8(out, size, &o, cp, &ok);
            continue;
        }
        const char* from = "\"\\/bfnrt";
        const char* to = "\"\\/\b\f\n\r\t";
        if (o + 1 >= size) { ok = false; break; }
        out[o++] = to[strchr(from, c) - from];
    }
    out[o] = 0;
    return ok;
}

f64 json_number(const JsonDoc* doc, u32 node) {
    if (node >= doc->count || doc->nodes[node].type != JSON_NUMBER) return 0.0;
    char buf[64];
    u32 n = doc->nodes[node].len;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, doc->text + doc->nodes[node].start, n);
    buf[n] = 0;
    return strtod(buf, nullptr);
}

bool json_true(const JsonDoc* doc, u32 node) {
    return node < doc->count && doc->nodes[node].type == JSON_BOOL && doc->text[doc->nodes[node].start] == 't';
}
