#pragma once
#include "core/types.h"

// Page-aligned, committed and pre-faulted allocation (VirtualAlloc). Null on failure.
void* big_alloc(u64 bytes);
void big_free(void* p);
