#pragma once
// Minimal ZIP reader for a small archive held in memory: finds a member by name in the central directory and
// extracts it (stored or deflate, RFC 1951), checking its size and CRC-32. No ZIP64, no encryption, no
// multi-disk archives. Written for the PawnIO modules download (core/pawnio_modules.cpp), whose zip is checked
// against a pinned SHA-256 before it gets here; still bounds-checks everything, so a bad archive just fails.
#include "core/types.h"

struct ZipEntry {
    u32 method;         // 0 = stored, 8 = deflate
    u32 crc;
    u32 comp_size;
    u32 size;           // uncompressed
    u32 data_offset;    // compressed bytes inside the archive
};

bool zip_find(const u8* zip, u32 zip_size, const char* name, ZipEntry* e);
// out must hold e->size bytes. False on a corrupt member, an unsupported method or a CRC mismatch.
bool zip_read(const u8* zip, u32 zip_size, const ZipEntry* e, u8* out);
u32 crc32_ieee(const void* data, u32 size);
