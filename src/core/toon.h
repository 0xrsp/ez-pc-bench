#pragma once
// Minimal TOON (Token-Oriented Object Notation) writer and line reader for our own result files.
// Subset used: 2-space indentation, `key: value`, `key:` (nested object), tabular arrays
// `key[N]{f1,f2,...}:` followed by N comma-separated rows one level deeper, and inline arrays
// `key[N]: a,b,c`. Strings are written bare unless they need quotes (see toon.cpp); quoted strings use
// \" \\ \n \r \t escapes. Numbers are plain decimals, booleans true / false, missing numbers null.
#include "core/types.h"
#include <stdio.h>

// ---- writer -------------------------------------------------------------------------------------

void toon_key(FILE* f, u32 depth, const char* key);                        // "key:" (nested object follows)
void toon_str(FILE* f, u32 depth, const char* key, const char* value);
void toon_num(FILE* f, u32 depth, const char* key, f64 value);
void toon_bool(FILE* f, u32 depth, const char* key, bool value);
void toon_table(FILE* f, u32 depth, const char* key, u32 rows, const char* fields);   // "key[N]{fields}:"

// One row of a table (or the values of an inline array): cells are separated by commas.
struct ToonRow {
    FILE* f;
    u32 cells;
};
ToonRow toon_row(FILE* f, u32 depth);       // starts a table row at `depth` (rows sit one level below the header)
void toon_cell_str(ToonRow* r, const char* s);
void toon_cell_num(ToonRow* r, f64 v);
void toon_cell_bool(ToonRow* r, bool v);
void toon_row_end(ToonRow* r);

// ---- reader -------------------------------------------------------------------------------------

constexpr u32 TOON_MAX_FIELDS = 24;
constexpr u32 TOON_CELL = 256;

struct ToonReader {
    const char* p;
    const char* end;
    u32 line;       // 1-based number of the last line returned
};

// A `key...: value` line.
struct ToonLine {
    char key[64];
    bool array;                                 // key[N]... form
    u32 count;                                  // N
    u32 field_count;                            // fields of a tabular header {a,b,...}; 0 otherwise
    char fields[TOON_MAX_FIELDS][32];
    const char* value;                          // text after ": " (empty for nested objects / table headers)
    const char* value_end;
};

// Reads a whole file (up to 64 MiB) and points `r` at it, past a UTF-8 BOM. Returns the buffer to pass to
// toon_free_file, or null with `err` set.
char* toon_read_file(const wchar_t* path, ToonReader* r, char* err, u32 err_size);
void toon_free_file(char* buf);

// Next non-blank line without its indentation and line break. False at the end of the input.
bool toon_next_line(ToonReader* r, const char** s, const char** e, u32* indent);
// Parses a key line. False if the line has no `key:` part.
bool toon_parse_key(const char* s, const char* e, ToonLine* out);
// Splits comma-separated cells (quoted cells may contain commas) and decodes each. Returns the cell count.
u32 toon_split(const char* s, const char* e, char cells[][TOON_CELL], u32 max);
// Decodes one scalar (quoted or bare) into out.
void toon_decode(const char* s, const char* e, char* out, u32 size);
