// Build tool: packs the reference runs in library/*.toon.txt into one compressed blob that the GUI embeds as
// a resource (see src/app/library.cpp for the reader).
// Layout before compression: "EZBL", u32 version, u32 count, then per run: u32 name_len, name (UTF-8 file
// name without .toon.txt), u32 data_len, data. Compressed as a whole with the Windows Compression API (LZMS,
// buffer mode: the stream header carries the original size).
// Usage: ezb_pack <out.bin> <file.toon.txt>...
#include "core/types.h"
#include <windows.h>
#include <compressapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* s_raw;
static u32 s_len, s_cap;

static void put(const void* p, u32 n) {
    if (s_len + n > s_cap) {
        s_cap = (s_len + n) * 2 + 4096;
        s_raw = (char*)realloc(s_raw, s_cap);
        if (!s_raw) { fprintf(stderr, "ezb_pack: out of memory\n"); exit(1); }
    }
    memcpy(s_raw + s_len, p, n);
    s_len += n;
}
static void put_u32(u32 v) { put(&v, 4); }

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fprintf(stderr, "usage: ezb_pack <out.bin> <file.toon.txt>...\n"); return 2; }
    put("EZBL", 4);
    put_u32(1);
    put_u32((u32)(argc - 2));
    for (int i = 2; i < argc; ++i) {
        FILE* f = _wfopen(argv[i], L"rb");
        if (!f) { fwprintf(stderr, L"ezb_pack: cannot open %ls\n", argv[i]); return 1; }
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* data = (char*)malloc(size > 0 ? (size_t)size : 1);
        const bool ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
        fclose(f);
        if (!ok) { fwprintf(stderr, L"ezb_pack: cannot read %ls\n", argv[i]); return 1; }

        const wchar_t* base = argv[i];
        for (const wchar_t* p = argv[i]; *p; ++p)
            if (*p == L'\\' || *p == L'/') base = p + 1;
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr);
        if (char* ext = strstr(name, ".toon.txt")) *ext = 0;
        put_u32((u32)strlen(name));
        put(name, (u32)strlen(name));
        put_u32((u32)size);
        put(data, (u32)size);
        free(data);
    }

    COMPRESSOR_HANDLE c = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &c)) { fprintf(stderr, "ezb_pack: CreateCompressor failed\n"); return 1; }
    SIZE_T cap = s_len + s_len / 2 + 65536, out_len = 0;
    void* out = malloc(cap);
    if (!out || !Compress(c, s_raw, s_len, out, cap, &out_len)) {
        fprintf(stderr, "ezb_pack: Compress failed (%lu)\n", GetLastError());
        return 1;
    }
    CloseCompressor(c);

    FILE* f = _wfopen(argv[1], L"wb");
    if (!f || fwrite(out, 1, out_len, f) != out_len) { fwprintf(stderr, L"ezb_pack: cannot write %ls\n", argv[1]); return 1; }
    fclose(f);
    printf("ezb_pack: %d runs, %u -> %zu bytes\n", argc - 2, s_len, (size_t)out_len);
    return 0;
}
