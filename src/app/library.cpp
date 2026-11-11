// Built-in reference runs: RCDATA 103 holds the LZMS-compressed blob written by tools/pack_library.cpp.
#include "app/library.h"
#include <windows.h>
#include <compressapi.h>
#include <string.h>

constexpr u32 LIBRARY_MAX = 256;
static LibraryRun s_runs[LIBRARY_MAX];
static u32 s_count;
static bool s_loaded;

static bool read_u32(const char** p, const char* end, u32* v) {
    if (end - *p < 4) return false;
    memcpy(v, *p, 4);
    *p += 4;
    return true;
}

u32 library_load() {
    if (s_loaded) return s_count;
    s_loaded = true;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(103), MAKEINTRESOURCEW(10));     // 10 = RT_RCDATA
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* packed = mem ? LockResource(mem) : nullptr;
    const DWORD packed_size = res ? SizeofResource(nullptr, res) : 0;
    if (!packed || !packed_size) return 0;

    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d)) return 0;
    SIZE_T raw_size = 0;
    Decompress(d, packed, packed_size, nullptr, 0, &raw_size);     // fails with the needed size
    // Kept for the app's lifetime: the run names and texts point into it.
    char* raw = raw_size ? (char*)VirtualAlloc(nullptr, raw_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
    const bool ok = raw && Decompress(d, packed, packed_size, raw, raw_size, &raw_size);
    CloseDecompressor(d);
    if (!ok) return 0;

    const char* p = raw;
    const char* end = raw + raw_size;
    u32 version = 0, count = 0;
    if (end - p < 4 || memcmp(p, "EZBL", 4) != 0) return 0;
    p += 4;
    if (!read_u32(&p, end, &version) || version != 1 || !read_u32(&p, end, &count)) return 0;
    static char names[LIBRARY_MAX][128];
    for (u32 i = 0; i < count && s_count < LIBRARY_MAX; ++i) {
        u32 name_len = 0, data_len = 0;
        if (!read_u32(&p, end, &name_len) || (u32)(end - p) < name_len) break;
        char* name = names[s_count];
        const u32 n = name_len < sizeof(names[0]) - 1 ? name_len : (u32)sizeof(names[0]) - 1;
        memcpy(name, p, n);
        name[n] = 0;
        p += name_len;
        if (!read_u32(&p, end, &data_len) || (u32)(end - p) < data_len) break;
        LibraryRun* r = &s_runs[s_count];
        r->name = name;
        r->data = p;
        r->size = data_len;
        p += data_len;
        char err[64];
        if (compare_peek(r->data, r->size, &r->info, err, sizeof(err))) s_count++;
    }
    return s_count;
}

const LibraryRun* library_run(u32 i) { return i < s_count ? &s_runs[i] : nullptr; }
