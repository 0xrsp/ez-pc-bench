#include "core/toon.h"
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---- writer -------------------------------------------------------------------------------------

static void indent(FILE* f, u32 depth) {
    for (u32 i = 0; i < depth; ++i) fputs("  ", f);
}

// Bare strings must not be confused with numbers / literals or with TOON syntax.
static bool needs_quotes(const char* s) {
    if (!s || !*s) return true;
    const size_t n = strlen(s);
    if (s[0] == ' ' || s[n - 1] == ' ' || s[0] == '-') return true;
    if (!strcmp(s, "true") || !strcmp(s, "false") || !strcmp(s, "null")) return true;
    char* stop = nullptr;
    strtod(s, &stop);
    if (stop && *stop == 0) return true;        // would read back as a number
    for (const char* p = s; *p; ++p)
        if ((unsigned char)*p < 0x20 || strchr(",:\"\\[]{}#", *p)) return true;
    return false;
}

static void write_str(FILE* f, const char* s) {
    if (!needs_quotes(s)) { fputs(s, f); return; }
    fputc('"', f);
    for (; s && *s; ++s) {
        switch (*s) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default: if ((unsigned char)*s >= 0x20) fputc(*s, f);
        }
    }
    fputc('"', f);
}

// Plain decimal (no exponent), shortest form that keeps ~9 significant digits.
static void write_num(FILE* f, f64 v) {
    if (!isfinite(v)) { fputs("null", f); return; }
    if (v == 0.0) { fputc('0', f); return; }
    if (v == floor(v) && fabs(v) < 1e15) { fprintf(f, "%.0f", v); return; }
    const i32 digits = 9 - (i32)floor(log10(fabs(v))) - 1;     // decimals for 9 significant digits
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits < 0 ? 0 : digits > 15 ? 15 : digits, v);
    if (strchr(buf, '.')) {     // trim trailing zeros
        char* end = buf + strlen(buf) - 1;
        while (*end == '0') *end-- = 0;
        if (*end == '.') *end = 0;
    }
    fputs(buf, f);
}

void toon_key(FILE* f, u32 depth, const char* key) {
    indent(f, depth);
    fprintf(f, "%s:\n", key);
}

void toon_str(FILE* f, u32 depth, const char* key, const char* value) {
    indent(f, depth);
    fprintf(f, "%s: ", key);
    write_str(f, value);
    fputc('\n', f);
}

void toon_num(FILE* f, u32 depth, const char* key, f64 value) {
    indent(f, depth);
    fprintf(f, "%s: ", key);
    write_num(f, value);
    fputc('\n', f);
}

void toon_bool(FILE* f, u32 depth, const char* key, bool value) {
    indent(f, depth);
    fprintf(f, "%s: %s\n", key, value ? "true" : "false");
}

void toon_table(FILE* f, u32 depth, const char* key, u32 rows, const char* fields) {
    indent(f, depth);
    fprintf(f, "%s[%u]{%s}:\n", key, rows, fields);
}

ToonRow toon_row(FILE* f, u32 depth) {
    indent(f, depth);
    return ToonRow{ f, 0 };
}

void toon_cell_str(ToonRow* r, const char* s) {
    if (r->cells++) fputc(',', r->f);
    write_str(r->f, s);
}

void toon_cell_num(ToonRow* r, f64 v) {
    if (r->cells++) fputc(',', r->f);
    write_num(r->f, v);
}

void toon_cell_bool(ToonRow* r, bool v) {
    if (r->cells++) fputc(',', r->f);
    fputs(v ? "true" : "false", r->f);
}

void toon_row_end(ToonRow* r) { fputc('\n', r->f); }

// ---- reader -------------------------------------------------------------------------------------

char* toon_read_file(const wchar_t* path, ToonReader* r, char* err, u32 err_size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { snprintf(err, err_size, "cannot open file (error %lu)", GetLastError()); return nullptr; }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 64 * (i64)MiB) {
        CloseHandle(f);
        snprintf(err, err_size, "unexpected file size");
        return nullptr;
    }
    char* buf = (char*)VirtualAlloc(nullptr, (SIZE_T)size.QuadPart + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0;
    const bool read = buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart;
    CloseHandle(f);
    if (!read) {
        toon_free_file(buf);
        snprintf(err, err_size, "read failed");
        return nullptr;
    }
    *r = { buf, buf + got, 0 };
    if (got >= 3 && !memcmp(buf, "\xEF\xBB\xBF", 3)) r->p += 3;
    return buf;
}

void toon_free_file(char* buf) {
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
}

bool toon_next_line(ToonReader* r, const char** s, const char** e, u32* indent_out) {
    while (r->p < r->end) {
        const char* line = r->p;
        const char* nl = (const char*)memchr(line, '\n', (size_t)(r->end - line));
        const char* line_end = nl ? nl : r->end;
        r->p = nl ? nl + 1 : r->end;
        r->line++;
        if (line_end > line && line_end[-1] == '\r') --line_end;
        u32 n = 0;
        while (line + n < line_end && line[n] == ' ') ++n;
        if (line + n == line_end) continue;         // blank
        *s = line + n;
        *e = line_end;
        *indent_out = n;
        return true;
    }
    return false;
}

void toon_decode(const char* s, const char* e, char* out, u32 size) {
    while (s < e && *s == ' ') ++s;
    while (e > s && e[-1] == ' ') --e;
    u32 n = 0;
    if (s < e && *s == '"') {
        ++s;
        while (s < e && *s != '"') {
            char c = *s++;
            if (c == '\\' && s < e) {
                c = *s++;
                c = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;    // \" \\ stay as-is
            }
            if (n + 1 < size) out[n++] = c;
        }
    } else {
        while (s < e && n + 1 < size) out[n++] = *s++;
    }
    if (size) out[n] = 0;
}

// End of the cell starting at s (the next comma outside quotes, or e).
static const char* cell_end(const char* s, const char* e) {
    bool quoted = false;
    for (; s < e; ++s) {
        if (quoted && *s == '\\') { ++s; continue; }
        if (*s == '"') quoted = !quoted;
        else if (*s == ',' && !quoted) return s;
    }
    return e;
}

u32 toon_split(const char* s, const char* e, char cells[][TOON_CELL], u32 max) {
    u32 n = 0;
    while (n < max) {
        const char* ce = cell_end(s, e);
        toon_decode(s, ce, cells[n++], TOON_CELL);
        if (ce >= e) break;
        s = ce + 1;
    }
    return n;
}

bool toon_parse_key(const char* s, const char* e, ToonLine* out) {
    memset(out, 0, sizeof(*out));
    // The key part ends at the first ':' outside quotes / brackets / braces.
    const char* colon = nullptr;
    bool quoted = false;
    u32 nest = 0;
    for (const char* p = s; p < e && !colon; ++p) {
        if (*p == '"') quoted = !quoted;
        else if (!quoted && (*p == '[' || *p == '{')) ++nest;
        else if (!quoted && (*p == ']' || *p == '}') && nest) --nest;
        else if (!quoted && !nest && *p == ':') colon = p;
    }
    if (!colon) return false;

    const char* bracket = (const char*)memchr(s, '[', (size_t)(colon - s));
    toon_decode(s, bracket ? bracket : colon, out->key, sizeof(out->key));
    if (bracket) {
        out->array = true;
        out->count = (u32)strtoul(bracket + 1, nullptr, 10);
        const char* brace = (const char*)memchr(bracket, '{', (size_t)(colon - bracket));
        if (brace) {
            const char* close = (const char*)memchr(brace, '}', (size_t)(colon - brace));
            if (!close) return false;
            char cells[TOON_MAX_FIELDS][TOON_CELL];
            out->field_count = toon_split(brace + 1, close, cells, TOON_MAX_FIELDS);
            for (u32 i = 0; i < out->field_count; ++i) {
                strncpy(out->fields[i], cells[i], sizeof(out->fields[i]) - 1);
                out->fields[i][sizeof(out->fields[i]) - 1] = 0;
            }
        }
    }
    out->value = colon + 1;
    if (out->value < e && *out->value == ' ') ++out->value;
    out->value_end = e;
    return true;
}
