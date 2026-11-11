// SHA-256 (SHA-NI) and AES-128 (AES-NI / VAES-512) kernels with known-answer self-tests.
#include "cpu/cpu_internal.h"
#include <immintrin.h>
#include <string.h>

// ---- SHA-256 ------------------------------------------------------------------------

alignas(16) static const u32 SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

__attribute__((target("sha"))) void sha256_blocks(u32 state[8], const u8* data, u64 len) {
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128((const __m128i*)&state[0]);
    __m128i s1 = _mm_loadu_si128((const __m128i*)&state[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);              // CDAB
    s1 = _mm_shuffle_epi32(s1, 0x1B);                // EFGH
    __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);        // ABEF
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);             // CDGH

    for (; len >= 64; len -= 64, data += 64) {
        const __m128i abef = s0, cdgh = s1;
        __m128i m[4];
        // 16 groups of 4 rounds; message schedule rotates through m[0..3].
#pragma clang loop unroll(full)
        for (u32 g = 0; g < 16; ++g) {
            if (g < 4) m[g] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16 * g)), MASK);
            __m128i msg = _mm_add_epi32(m[g & 3], _mm_load_si128((const __m128i*)&SHA_K[4 * g]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
            if (g >= 3 && g <= 14) {
                __m128i t = _mm_alignr_epi8(m[g & 3], m[(g - 1) & 3], 4);
                m[(g + 1) & 3] = _mm_sha256msg2_epu32(_mm_add_epi32(m[(g + 1) & 3], t), m[g & 3]);
            }
            msg = _mm_shuffle_epi32(msg, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
            if (g >= 1 && g <= 12) m[(g - 1) & 3] = _mm_sha256msg1_epu32(m[(g - 1) & 3], m[g & 3]);
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
    }

    tmp = _mm_shuffle_epi32(s0, 0x1B);               // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xB1);                // DCHG
    s0 = _mm_blend_epi16(tmp, s1, 0xF0);             // DCBA
    s1 = _mm_alignr_epi8(s1, tmp, 8);                // ABEF -> HGFE
    _mm_storeu_si128((__m128i*)&state[0], s0);
    _mm_storeu_si128((__m128i*)&state[4], s1);
}

bool sha256_selftest() {
    // SHA-256("abc"), single padded block.
    u8 block[64] = { 'a', 'b', 'c', 0x80 };
    block[63] = 24;
    u32 st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    sha256_blocks(st, block, 64);
    static const u32 expect[8] = { 0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
                                   0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad };
    return memcmp(st, expect, sizeof(st)) == 0;
}

// ---- AES-128 --------------------------------------------------------------------------

__attribute__((target("aes"))) static __m128i aes_expand_step(__m128i key, __m128i gen) {
    gen = _mm_shuffle_epi32(gen, _MM_SHUFFLE(3, 3, 3, 3));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    return _mm_xor_si128(key, gen);
}

__attribute__((target("aes"))) void aes128_expand(const u8 key[16], AesKey* out) {
    __m128i* rk = (__m128i*)out->rk;
    rk[0] = _mm_loadu_si128((const __m128i*)key);
    rk[1] = aes_expand_step(rk[0], _mm_aeskeygenassist_si128(rk[0], 0x01));
    rk[2] = aes_expand_step(rk[1], _mm_aeskeygenassist_si128(rk[1], 0x02));
    rk[3] = aes_expand_step(rk[2], _mm_aeskeygenassist_si128(rk[2], 0x04));
    rk[4] = aes_expand_step(rk[3], _mm_aeskeygenassist_si128(rk[3], 0x08));
    rk[5] = aes_expand_step(rk[4], _mm_aeskeygenassist_si128(rk[4], 0x10));
    rk[6] = aes_expand_step(rk[5], _mm_aeskeygenassist_si128(rk[5], 0x20));
    rk[7] = aes_expand_step(rk[6], _mm_aeskeygenassist_si128(rk[6], 0x40));
    rk[8] = aes_expand_step(rk[7], _mm_aeskeygenassist_si128(rk[7], 0x80));
    rk[9] = aes_expand_step(rk[8], _mm_aeskeygenassist_si128(rk[8], 0x1b));
    rk[10] = aes_expand_step(rk[9], _mm_aeskeygenassist_si128(rk[9], 0x36));
}

__attribute__((target("aes"))) static __m128i aes_encrypt_block(const AesKey* k, __m128i b) {
    const __m128i* rk = (const __m128i*)k->rk;
    b = _mm_xor_si128(b, rk[0]);
    for (int r = 1; r < 10; ++r) b = _mm_aesenc_si128(b, rk[r]);
    return _mm_aesenclast_si128(b, rk[10]);
}

// 8 blocks in flight hides the 4-cycle aesenc latency on two pipes.
__attribute__((target("aes"))) void aes_ctr_ni(const AesKey* k, u64 ctr0, const u8* src, u8* dst, u64 bytes) {
    const __m128i* rk = (const __m128i*)k->rk;
    __m128i ctr = _mm_set_epi64x(0, (long long)ctr0);
    const __m128i one = _mm_set_epi64x(0, 1);
    for (u64 off = 0; off < bytes; off += 128) {
        __m128i b[8];
        for (int i = 0; i < 8; ++i) { b[i] = _mm_xor_si128(ctr, rk[0]); ctr = _mm_add_epi64(ctr, one); }
        for (int r = 1; r < 10; ++r)
            for (int i = 0; i < 8; ++i) b[i] = _mm_aesenc_si128(b[i], rk[r]);
        for (int i = 0; i < 8; ++i) {
            b[i] = _mm_aesenclast_si128(b[i], rk[10]);
            __m128i in = _mm_loadu_si128((const __m128i*)(src + off + 16 * i));
            _mm_storeu_si128((__m128i*)(dst + off + 16 * i), _mm_xor_si128(b[i], in));
        }
    }
}

// 4 zmm x 4 blocks = 16 blocks per iteration.
__attribute__((target("vaes,avx512f"))) void aes_ctr_vaes512(const AesKey* k, u64 ctr0, const u8* src, u8* dst, u64 bytes) {
    const __m128i* rk = (const __m128i*)k->rk;
    __m512i kr[11];
    for (int r = 0; r < 11; ++r) kr[r] = _mm512_broadcast_i32x4(rk[r]);
    const long long c = (long long)ctr0;
    __m512i ctr = _mm512_set_epi64(0, c + 3, 0, c + 2, 0, c + 1, 0, c);
    const __m512i four = _mm512_set_epi64(0, 4, 0, 4, 0, 4, 0, 4);
    for (u64 off = 0; off < bytes; off += 256) {
        __m512i b[4];
        for (int i = 0; i < 4; ++i) { b[i] = _mm512_xor_si512(ctr, kr[0]); ctr = _mm512_add_epi64(ctr, four); }
        for (int r = 1; r < 10; ++r)
            for (int i = 0; i < 4; ++i) b[i] = _mm512_aesenc_epi128(b[i], kr[r]);
        for (int i = 0; i < 4; ++i) {
            b[i] = _mm512_aesenclast_epi128(b[i], kr[10]);
            __m512i in = _mm512_loadu_si512(src + off + 64 * i);
            _mm512_storeu_si512(dst + off + 64 * i, _mm512_xor_si512(b[i], in));
        }
    }
}

bool aes_selftest(bool check_vaes512) {
    // FIPS-197 appendix C.1
    static const u8 key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    static const u8 pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const u8 ct[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                               0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    AesKey k;
    aes128_expand(key, &k);
    alignas(16) u8 out[16];
    _mm_storeu_si128((__m128i*)out, aes_encrypt_block(&k, _mm_loadu_si128((const __m128i*)pt)));
    if (memcmp(out, ct, 16) != 0) return false;
    if (!check_vaes512) return true;

    // VAES path must match the AES-NI path.
    static u8 src[4096], a[4096], b[4096];
    for (u32 i = 0; i < sizeof(src); ++i) src[i] = (u8)(i * 31 + 7);
    aes_ctr_ni(&k, 1234, src, a, sizeof(src));
    aes_ctr_vaes512(&k, 1234, src, b, sizeof(src));
    return memcmp(a, b, sizeof(a)) == 0;
}
