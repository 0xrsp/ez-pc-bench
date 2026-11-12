#pragma once
// Minimal JSON reader: one pass over the text into a flat node array (document order), no allocations.
// A node's children follow it directly; `end` is the index just past its subtree, so the next sibling of
// node i is nodes[i].end. Object children alternate key (string) / value. Strings keep their raw span
// (escapes are decoded on demand by json_string).
#include "core/types.h"

enum JsonType : u8 { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct JsonNode {
    JsonType type;
    u32 start, len;     // raw text span (strings: between the quotes)
    u32 end;            // index of the node after this subtree
};

struct JsonDoc {
    const char* text;
    JsonNode* nodes;
    u32 count, cap;
};

constexpr u32 JSON_NONE = ~0u;

// Parses `len` bytes into `nodes` (capacity `cap`; `len` nodes are always enough). Root is node 0.
bool json_parse(JsonDoc* doc, const char* text, u32 len, JsonNode* nodes, u32 cap);

u32 json_find(const JsonDoc* doc, u32 obj, const char* key);   // value index of `key`, or JSON_NONE
u32 json_first(const JsonDoc* doc, u32 node);                    // first array element, or JSON_NONE
u32 json_next(const JsonDoc* doc, u32 parent, u32 elem);         // next array element, or JSON_NONE
// Decoded string (escapes, \uXXXX to UTF-8), always 0-terminated; false if not a string or truncated.
bool json_string(const JsonDoc* doc, u32 node, char* out, u32 size);
f64 json_number(const JsonDoc* doc, u32 node);                   // 0 if not a number
bool json_true(const JsonDoc* doc, u32 node);
