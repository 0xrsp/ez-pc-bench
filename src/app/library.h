#pragma once
// Built-in library of reference runs: library/*.toon.txt packed and compressed into the GUI exe at build
// time (tools/pack_library.cpp), decompressed on first use.
#include "core/compare.h"

struct LibraryRun {
    const char* name;           // file name without .toon.txt
    const char* data;           // results text (TOON)
    u32 size;
    CmpRun info;                // system info for the listing (compare_peek)
};

// Decompresses the library once. Returns the run count (0 if the exe has none or it is damaged).
u32 library_load();
const LibraryRun* library_run(u32 i);
