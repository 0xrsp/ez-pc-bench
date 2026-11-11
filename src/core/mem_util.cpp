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

static bool enable_lock_memory_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

void* big_alloc_large(u64 bytes) {
    const u64 page = GetLargePageMinimum();
    if (!page || !enable_lock_memory_privilege()) return nullptr;
    bytes = (bytes + page - 1) / page * page;
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
}
