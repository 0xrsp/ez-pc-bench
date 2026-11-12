// Checksums and ciphers: CRC32C (SSE4.2 crc32 instruction, one stream), xxHash64 (scalar, four lanes),
// AES-128-GCM through Windows CNG (BCrypt: what SChannel / TLS on Windows uses) and our own ChaCha20 (AVX2,
// eight blocks at a time), cross-checked against CNG's ChaCha20-Poly1305. Each has a known-answer test.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <windows.h>
#include <bcrypt.h>
#include <immintrin.h>
#include <string.h>

static constexpr u64 HASH_BYTES = 64 * KiB;    // per thread and call, L2 resident

struct HashCtx {
    u8* buf;            // per thread: 2 * HASH_BYTES (source, destination)
    BCRYPT_KEY_HANDLE key[MAX_LP];
    std::atomic<bool> failed;
};

static u8* src_of(HashCtx* c, u32 index) { return c->buf + (u64)index * 2 * HASH_BYTES; }

static bool hash_alloc(HashCtx* c, u32 threads) {
    c->buf = (u8*)big_alloc((u64)threads * 2 * HASH_BYTES);
    if (!c->buf) return false;
    for (u64 i = 0; i < (u64)threads * 2 * HASH_BYTES; ++i) c->buf[i] = (u8)(i * 2654435761u >> 11);
    c->failed.store(false);
    return true;
}

// ---- CRC32C ----------------------------------------------------------------------------------------------

static u32 crc32c(u32 crc, const u8* p, u64 n) {
    u64 c = ~crc;
    for (; n >= 8; n -= 8, p += 8) { u64 v; memcpy(&v, p, 8); c = _mm_crc32_u64(c, v); }
    for (; n; --n) c = _mm_crc32_u8((u32)c, *p++);
    return ~(u32)c;
}

static f64 kern_crc32c(void* ctx, u32 index, u32) {
    do_not_optimize(crc32c(0, src_of((HashCtx*)ctx, index), HASH_BYTES));
    return (f64)HASH_BYTES;
}

void cpu_crc32c(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!g_sys.cpu.sse42) { res_skip(out, "CPU lacks SSE4.2"); return; }
    if (crc32c(0, (const u8*)"123456789", 9) != 0xE3069283u) { res_fail(out, "CRC32C known-answer test failed"); return; }
    static HashCtx c;
    if (!hash_alloc(&c, thread_count_for(p->thread_count))) { res_fail(out, "out of memory"); return; }
    st_mt_test(kern_crc32c, &c, "GB/s", 1e9, p, live, out, cancel);
    big_free(c.buf);
}

// ---- xxHash64 ------------------------------------------------------------------------------------------

static constexpr u64 XP1 = 0x9E3779B185EBCA87ull, XP2 = 0xC2B2AE3D27D4EB4Full, XP3 = 0x165667B19E3779F9ull,
                     XP4 = 0x85EBCA77C2B2AE63ull, XP5 = 0x27D4EB2F165667C5ull;

static inline u64 rotl64(u64 x, u32 r) { return (x << r) | (x >> (64 - r)); }
static inline u64 xx_round(u64 acc, u64 in) { return rotl64(acc + in * XP2, 31) * XP1; }
static inline u64 xx_merge(u64 h, u64 v) { return (h ^ xx_round(0, v)) * XP1 + XP4; }
static inline u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }

FORCE_INLINE static u64 xxh64_body(const u8* p, u64 n, u64 seed) {
    const u8* const end = p + n;
    u64 h;
    if (n >= 32) {
        u64 v1 = seed + XP1 + XP2, v2 = seed + XP2, v3 = seed, v4 = seed - XP1;
        for (; end - p >= 32; p += 32) {
            v1 = xx_round(v1, rd64(p));
            v2 = xx_round(v2, rd64(p + 8));
            v3 = xx_round(v3, rd64(p + 16));
            v4 = xx_round(v4, rd64(p + 24));
        }
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xx_merge(xx_merge(xx_merge(xx_merge(h, v1), v2), v3), v4);
    } else {
        h = seed + XP5;
    }
    h += n;
    for (; end - p >= 8; p += 8) h = rotl64(h ^ xx_round(0, rd64(p)), 27) * XP1 + XP4;
    if (end - p >= 4) { h = rotl64(h ^ (rd32(p) * XP1), 23) * XP2 + XP3; p += 4; }
    for (; p < end; ++p) h = rotl64(h ^ (*p * XP5), 11) * XP1;
    h ^= h >> 33; h *= XP2;
    h ^= h >> 29; h *= XP3;
    h ^= h >> 32;
    return h;
}

TARGET_AVX2 static u64 xxh64_avx2(const u8* p, u64 n, u64 seed) { return xxh64_body(p, n, seed); }
static u64 xxh64_base(const u8* p, u64 n, u64 seed) { return xxh64_body(p, n, seed); }
static u64 xxh64(const u8* p, u64 n, u64 seed) { return g_sys.cpu.avx2 ? xxh64_avx2(p, n, seed) : xxh64_base(p, n, seed); }

static bool xxh64_selftest() {
    // Reference values: xxHash test vectors and the python-xxhash documentation example (39 bytes: covers the
    // 32-byte stripes and the 8 / 4 / 1-byte tails).
    static const char SPAM[] = "Nobody inspects the spammish repetition";
    return xxh64(nullptr, 0, 0) == 0xEF46DB3751D8E999ull && xxh64((const u8*)"abc", 3, 0) == 0x44BC2CF5AD770999ull &&
           xxh64((const u8*)SPAM, sizeof(SPAM) - 1, 0) == 0xFBCEA83C8A378BF1ull;
}

static f64 kern_xxh64(void* ctx, u32 index, u32) {
    do_not_optimize(xxh64(src_of((HashCtx*)ctx, index), HASH_BYTES, index));
    return (f64)HASH_BYTES;
}

void cpu_xxhash(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (!xxh64_selftest()) { res_fail(out, "xxHash64 known-answer test failed"); return; }
    static HashCtx c;
    if (!hash_alloc(&c, thread_count_for(p->thread_count))) { res_fail(out, "out of memory"); return; }
    st_mt_test(kern_xxh64, &c, "GB/s", 1e9, p, live, out, cancel);
    big_free(c.buf);
}

// ---- AES-128-GCM via CNG --------------------------------------------------------------------------------

static BCRYPT_KEY_HANDLE gcm_key(BCRYPT_ALG_HANDLE alg, const u8 key[16]) {
    BCRYPT_KEY_HANDLE k = nullptr;
    return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 16, 0)) ? k : nullptr;
}

static bool gcm_encrypt(BCRYPT_KEY_HANDLE key, const u8 nonce[12], const u8* src, u8* dst, u32 n, u8 tag[16]) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce;
    info.cbNonce = 12;
    info.pbTag = tag;
    info.cbTag = 16;
    ULONG done = 0;
    return BCRYPT_SUCCESS(BCryptEncrypt(key, (PUCHAR)src, n, &info, nullptr, 0, dst, n, &done, 0)) && done == n;
}

static f64 kern_gcm(void* ctx, u32 index, u32) {
    HashCtx* c = (HashCtx*)ctx;
    u8 nonce[12] = { (u8)index }, tag[16];
    u8* s = src_of(c, index);
    if (!gcm_encrypt(c->key[index], nonce, s, s + HASH_BYTES, (u32)HASH_BYTES, tag)) c->failed.store(true);
    return (f64)HASH_BYTES;
}

void cpu_aes_gcm(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        res_skip(out, "CNG AES-GCM unavailable");
        return;
    }
    // NIST GCM test case 2: zero key, zero 96-bit IV, one zero block.
    static const u8 CT2[16] = { 0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92, 0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78 };
    static const u8 TAG2[16] = { 0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec, 0x13, 0xbd, 0xf5, 0x3a, 0x67, 0xb2, 0x12, 0x57, 0xbd, 0xdf };
    const u8 zero[16] = {}, nonce[12] = {};
    u8 ct[16], tag[16];
    BCRYPT_KEY_HANDLE k0 = gcm_key(alg, zero);
    const bool kat = k0 && gcm_encrypt(k0, nonce, zero, ct, 16, tag) && !memcmp(ct, CT2, 16) && !memcmp(tag, TAG2, 16);
    if (k0) BCryptDestroyKey(k0);
    static HashCtx c;
    const u32 mt = thread_count_for(p->thread_count);
    bool ok = kat && hash_alloc(&c, mt);
    static const u8 KEY[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    for (u32 i = 0; i < mt && ok; ++i) ok = (c.key[i] = gcm_key(alg, KEY)) != nullptr;
    if (ok) {
        st_mt_test(kern_gcm, &c, "GB/s", 1e9, p, live, out, cancel);
        if (c.failed.load()) res_fail(out, "BCryptEncrypt failed");
    } else {
        res_fail(out, kat ? "CNG key setup failed" : "AES-GCM known-answer test failed");
    }
    for (u32 i = 0; i < MAX_LP; ++i) if (c.key[i]) { BCryptDestroyKey(c.key[i]); c.key[i] = nullptr; }
    big_free(c.buf);
    c.buf = nullptr;
    BCryptCloseAlgorithmProvider(alg, 0);
}

// ---- ChaCha20 ------------------------------------------------------------------------------------------
// RFC 8439 block function; the AVX2 path computes 8 consecutive blocks with one block per 32-bit lane.

static inline u32 rotl32(u32 x, u32 r) { return (x << r) | (x >> (32 - r)); }

#define QR(a, b, c, d)                                   \
    a += b; d ^= a; d = rotl32(d, 16);                   \
    c += d; b ^= c; b = rotl32(b, 12);                   \
    a += b; d ^= a; d = rotl32(d, 8);                    \
    c += d; b ^= c; b = rotl32(b, 7);

static void chacha_init(u32 st[16], const u8 key[32], u32 counter, const u8 nonce[12]) {
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (u32 i = 0; i < 8; ++i) st[4 + i] = rd32(key + 4 * i);
    st[12] = counter;
    for (u32 i = 0; i < 3; ++i) st[13 + i] = rd32(nonce + 4 * i);
}

static void chacha_block(const u32 in[16], u32 out[16]) {
    u32 x[16];
    memcpy(x, in, sizeof(x));
    for (u32 r = 0; r < 10; ++r) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (u32 i = 0; i < 16; ++i) out[i] = x[i] + in[i];
}

// Scalar reference: dst = src ^ keystream, n multiple of 64.
static void chacha20_scalar(const u8 key[32], u32 counter, const u8 nonce[12], const u8* src, u8* dst, u64 n) {
    u32 st[16], ks[16];
    chacha_init(st, key, counter, nonce);
    for (u64 off = 0; off < n; off += 64, ++st[12]) {
        chacha_block(st, ks);
        for (u32 i = 0; i < 64; ++i) dst[off + i] = src[off + i] ^ ((const u8*)ks)[i];
    }
}

TARGET_AVX2 static inline __m256i rot16(__m256i x) {
    return _mm256_shuffle_epi8(x, _mm256_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2,
                                                  13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2));
}
TARGET_AVX2 static inline __m256i rot8(__m256i x) {
    return _mm256_shuffle_epi8(x, _mm256_set_epi8(14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3,
                                                  14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3));
}
TARGET_AVX2 static inline __m256i rotn(__m256i x, int n) { return _mm256_or_si256(_mm256_slli_epi32(x, n), _mm256_srli_epi32(x, 32 - n)); }

#define VQR(a, b, c, d)                                                                         \
    a = _mm256_add_epi32(a, b); d = rot16(_mm256_xor_si256(d, a));                              \
    c = _mm256_add_epi32(c, d); b = rotn(_mm256_xor_si256(b, c), 12);                          \
    a = _mm256_add_epi32(a, b); d = rot8(_mm256_xor_si256(d, a));                               \
    c = _mm256_add_epi32(c, d); b = rotn(_mm256_xor_si256(b, c), 7);

// Transposes 8 state words (one per block lane) and XORs them into the 8 output blocks at word offset `w0`.
TARGET_AVX2 static inline void chacha_store8(const __m256i x[8], const u8* src, u8* dst, u32 w0) {
    const __m256i t0 = _mm256_unpacklo_epi32(x[0], x[1]), t1 = _mm256_unpackhi_epi32(x[0], x[1]);
    const __m256i t2 = _mm256_unpacklo_epi32(x[2], x[3]), t3 = _mm256_unpackhi_epi32(x[2], x[3]);
    const __m256i t4 = _mm256_unpacklo_epi32(x[4], x[5]), t5 = _mm256_unpackhi_epi32(x[4], x[5]);
    const __m256i t6 = _mm256_unpacklo_epi32(x[6], x[7]), t7 = _mm256_unpackhi_epi32(x[6], x[7]);
    const __m256i u[4] = { _mm256_unpacklo_epi64(t0, t2), _mm256_unpackhi_epi64(t0, t2),
                           _mm256_unpacklo_epi64(t1, t3), _mm256_unpackhi_epi64(t1, t3) };   // blocks k | k+4, words 0-3
    const __m256i v[4] = { _mm256_unpacklo_epi64(t4, t6), _mm256_unpackhi_epi64(t4, t6),
                           _mm256_unpacklo_epi64(t5, t7), _mm256_unpackhi_epi64(t5, t7) };   // words 4-7
    for (u32 k = 0; k < 4; ++k) {
        const __m256i lo = _mm256_permute2x128_si256(u[k], v[k], 0x20), hi = _mm256_permute2x128_si256(u[k], v[k], 0x31);
        const u64 a = (u64)k * 64 + w0 * 4, b = (u64)(k + 4) * 64 + w0 * 4;
        _mm256_storeu_si256((__m256i*)(dst + a), _mm256_xor_si256(lo, _mm256_loadu_si256((const __m256i*)(src + a))));
        _mm256_storeu_si256((__m256i*)(dst + b), _mm256_xor_si256(hi, _mm256_loadu_si256((const __m256i*)(src + b))));
    }
}

// dst = src ^ keystream, n multiple of 512.
TARGET_AVX2 static void chacha20_avx2(const u8 key[32], u32 counter, const u8 nonce[12], const u8* src, u8* dst, u64 n) {
    u32 st[16];
    chacha_init(st, key, counter, nonce);
    __m256i in[16];
    for (u32 i = 0; i < 16; ++i) in[i] = _mm256_set1_epi32((int)st[i]);
    in[12] = _mm256_add_epi32(in[12], _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    for (u64 off = 0; off < n; off += 512) {
        __m256i x[16];
        for (u32 i = 0; i < 16; ++i) x[i] = in[i];
        for (u32 r = 0; r < 10; ++r) {
            VQR(x[0], x[4], x[8], x[12]) VQR(x[1], x[5], x[9], x[13]) VQR(x[2], x[6], x[10], x[14]) VQR(x[3], x[7], x[11], x[15])
            VQR(x[0], x[5], x[10], x[15]) VQR(x[1], x[6], x[11], x[12]) VQR(x[2], x[7], x[8], x[13]) VQR(x[3], x[4], x[9], x[14])
        }
        for (u32 i = 0; i < 16; ++i) x[i] = _mm256_add_epi32(x[i], in[i]);
        chacha_store8(x, src + off, dst + off, 0);
        chacha_store8(x + 8, src + off, dst + off, 8);
        in[12] = _mm256_add_epi32(in[12], _mm256_set1_epi32(8));
    }
}

static const u8 CHACHA_KEY[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

// RFC 8439 2.3.2 block (first state word), AVX2 == scalar, and scalar == CNG ChaCha20-Poly1305's ciphertext
// (which encrypts with block counter 1). CNG's check is skipped where the algorithm is unavailable.
static bool chacha_selftest(bool* cng_checked) {
    static const u8 nonce232[12] = { 0, 0, 0, 0x09, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    u32 st[16], ks[16];
    chacha_init(st, CHACHA_KEY, 1, nonce232);
    chacha_block(st, ks);
    if (ks[0] != 0xe4e7f110u) return false;

    static u8 src[4096], a[4096], b[4096];
    for (u32 i = 0; i < sizeof(src); ++i) src[i] = (u8)(i * 13 + 5);
    const u8 nonce[12] = { 7, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    chacha20_scalar(CHACHA_KEY, 1, nonce, src, a, sizeof(src));
    chacha20_avx2(CHACHA_KEY, 1, nonce, src, b, sizeof(src));
    if (memcmp(a, b, sizeof(a)) != 0) return false;

    *cng_checked = false;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(BCRYPT_CHACHA20_POLY1305_ALG_HANDLE, &key, nullptr, 0,
                                                  (PUCHAR)CHACHA_KEY, 32, 0))) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        u8 tag[16];
        info.pbNonce = (PUCHAR)nonce;
        info.cbNonce = 12;
        info.pbTag = tag;
        info.cbTag = 16;
        ULONG done = 0;
        if (BCRYPT_SUCCESS(BCryptEncrypt(key, src, sizeof(src), &info, nullptr, 0, b, sizeof(b), &done, 0))) {
            *cng_checked = true;
            if (done != sizeof(b) || memcmp(a, b, sizeof(a)) != 0) { BCryptDestroyKey(key); return false; }
        }
        BCryptDestroyKey(key);
    }
    return true;
}

static f64 kern_chacha(void* ctx, u32 index, u32) {
    HashCtx* c = (HashCtx*)ctx;
    const u8 nonce[12] = { (u8)index };
    u8* s = src_of(c, index);
    chacha20_avx2(CHACHA_KEY, 1, nonce, s, s + HASH_BYTES, HASH_BYTES);
    clobber_memory();
    return (f64)HASH_BYTES;
}

void cpu_chacha20(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    bool cng = false;
    if (!chacha_selftest(&cng)) { res_fail(out, "ChaCha20 known-answer / cross-check failed"); return; }
    static HashCtx c;
    if (!hash_alloc(&c, thread_count_for(p->thread_count))) { res_fail(out, "out of memory"); return; }
    st_mt_test(kern_chacha, &c, "GB/s", 1e9, p, live, out, cancel);
    big_free(c.buf);
    c.buf = nullptr;
}
