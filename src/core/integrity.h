#pragma once
// Result integrity (FUTURE.md section 11, P3): every results file ends with
//   integrity:
//     exe: ez-pc-bench.exe
//     exe_sha256: <SHA-256 of the exe that wrote it>
//     sha256: <SHA-256 of the file's lines, see below>
// The content hash covers every line except the top-level `score` and `integrity` blocks (the score is
// derived and may be rewritten by `--score-write`), each line without its line break plus "\n", so CRLF
// conversion doesn't break it. Anyone can recompute it: it spots accidental or casual edits, not forgery.
#include "core/types.h"

bool integrity_append(const wchar_t* path);     // after the file is written and closed
enum IntegrityState : i8 { INTEGRITY_NONE = -1, INTEGRITY_EDITED = 0, INTEGRITY_OK = 1 };
IntegrityState integrity_check(const char* data, u32 size);
const char* integrity_name(IntegrityState s);   // "not signed", "edited", "intact"
// SHA-256 of the running exe as 64 hex digits (computed once).
const char* integrity_exe_sha256();
// SHA-256 of a buffer as 64 lowercase hex digits; "" on failure.
void sha256_hex(const void* data, u32 size, char hex[65]);
