// Result integrity hashes (see integrity.h). SHA-256 via BCrypt (part of Windows).
#include "core/integrity.h"
#include "core/toon.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

struct Sha256 {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
};

static bool sha_begin(Sha256* s) {
    s->alg = nullptr;
    s->hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    return BCryptCreateHash(s->alg, &s->hash, nullptr, 0, nullptr, 0, 0) == 0;
}

static void sha_add(Sha256* s, const void* p, u32 n) {
    if (s->hash && n) BCryptHashData(s->hash, (PUCHAR)p, n, 0);
}

// Finishes into 64 hex digits + NUL; "" on failure.
static void sha_end(Sha256* s, char hex[65]) {
    u8 digest[32];
    hex[0] = 0;
    if (s->hash && BCryptFinishHash(s->hash, digest, sizeof(digest), 0) == 0)
        for (u32 i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (s->hash) BCryptDestroyHash(s->hash);
    if (s->alg) BCryptCloseAlgorithmProvider(s->alg, 0);
}

const char* integrity_exe_sha256() {
    static char hex[65];
    static bool done;
    if (done) return hex;
    done = true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return hex;
    Sha256 s;
    if (sha_begin(&s)) {
        static u8 buf[256 * 1024];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) sha_add(&s, buf, got);
    }
    sha_end(&s, hex);
    CloseHandle(f);
    return hex;
}

void sha256_hex(const void* data, u32 size, char hex[65]) {
    Sha256 s;
    if (sha_begin(&s)) sha_add(&s, data, size);
    sha_end(&s, hex);
}

static bool excluded_block(const char* s, const char* e) {
    static const char* const keys[] = { "score:", "integrity:" };
    for (u32 i = 0; i < ARRAY_COUNT(keys); ++i) {
        const size_t n = strlen(keys[i]);
        if ((size_t)(e - s) >= n && !memcmp(s, keys[i], n)) return true;
    }
    return false;
}

// Content hash; also returns the stored hash ("" if none) when `stored` is given.
static void content_hash(const char* data, u32 size, char hex[65], char stored[65]) {
    ToonReader r = { data, data + size, 0 };
    if (size >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) r.p += 3;
    if (stored) stored[0] = 0;
    Sha256 s;
    sha_begin(&s);
    bool skip = false, in_integrity = false;
    const char *b, *e;
    u32 indent;
    while (toon_next_line(&r, &b, &e, &indent)) {
        if (indent == 0) {
            skip = excluded_block(b, e);
            in_integrity = skip && b[0] == 'i';
        }
        if (in_integrity && stored && indent > 0) {
            ToonLine l;
            if (toon_parse_key(b, e, &l) && !strcmp(l.key, "sha256")) toon_decode(l.value, l.value_end, stored, 65);
        }
        if (skip) continue;
        // Indentation is part of the structure: hash it too.
        static const char spaces[64] = "                                                               ";
        sha_add(&s, spaces, indent < 63 ? indent : 63);
        sha_add(&s, b, (u32)(e - b));
        sha_add(&s, "\n", 1);
    }
    sha_end(&s, hex);
}

bool integrity_append(const wchar_t* path) {
    ToonReader r;
    char err[64];
    char* buf = toon_read_file(path, &r, err, sizeof(err));
    if (!buf) return false;
    char hex[65];
    content_hash(r.p, (u32)(r.end - r.p), hex, nullptr);
    toon_free_file(buf);
    FILE* f = _wfopen(path, L"ab");
    if (!f) return false;
    wchar_t exe[MAX_PATH];
    char exe8[MAX_PATH * 3];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : exe, -1, exe8, sizeof(exe8), nullptr, nullptr);
    toon_key(f, 0, "integrity");
    toon_str(f, 1, "exe", exe8);
    toon_str(f, 1, "exe_sha256", integrity_exe_sha256());
    toon_str(f, 1, "sha256", hex);
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

IntegrityState integrity_check(const char* data, u32 size) {
    char hex[65], stored[65];
    content_hash(data, size, hex, stored);
    if (!stored[0]) return INTEGRITY_NONE;
    return !strcmp(hex, stored) ? INTEGRITY_OK : INTEGRITY_EDITED;
}

const char* integrity_name(IntegrityState s) {
    return s == INTEGRITY_OK ? "intact" : s == INTEGRITY_EDITED ? "edited after it was written" : "no hash (older file)";
}
