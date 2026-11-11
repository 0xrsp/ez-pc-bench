#pragma once
#include "core/types.h"

// Page-aligned, committed and pre-faulted allocation (VirtualAlloc). Null on failure.
void* big_alloc(u64 bytes);
void big_free(void* p);

// Large-page allocation (needs SeLockMemoryPrivilege, which even admins lack by default).
// Returns null if unavailable; `bytes` is rounded up to the large-page size.
void* big_alloc_large(u64 bytes);
