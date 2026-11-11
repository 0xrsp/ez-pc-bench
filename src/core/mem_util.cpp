#include "core/mem_util.h"
#include <windows.h>

void* big_alloc(u64 bytes) {
    u8* p = (u8*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    for (u64 off = 0; off < bytes; off += 4096) p[off] = 0;   // pre-fault: Windows zeroes pages on first touch
    return p;
}

void big_free(void* p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}
