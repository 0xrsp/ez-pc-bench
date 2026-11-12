// Minimal ZIP reader + inflater (see unzip.h). The inflater follows RFC 1951 the way zlib's reference decoder
// puff.c does: canonical Huffman codes decoded bit by bit from per-length counts. Slow per byte, but it only
// ever unpacks a few hundred KB.
#include "core/unzip.h"
#include <string.h>

// ---- CRC-32 (IEEE, reflected 0xEDB88320) ------------------------------------------------------------------

struct Crc32Table {
    u32 t[256];
    constexpr Crc32Table() : t() {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
};
static constexpr Crc32Table CRC_TABLE;

u32 crc32_ieee(const void* data, u32 size) {
    const u8* p = (const u8*)data;
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < size; ++i) c = CRC_TABLE.t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- inflate ---------------------------------------------------------------------------------------------

struct Inflate {
    const u8* in;
    u32 in_size, in_pos;
    u32 bitbuf, bitcnt;
    bool err;           // ran out of input
    u8* out;
    u32 out_size, out_pos;
};

static u32 get_bits(Inflate* s, u32 need) {
    u32 v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_size) {
            s->err = true;
            return 0;
        }
        v |= (u32)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return v & ((1u << need) - 1);
}

constexpr u32 MAX_BITS = 15;

struct Huffman {
    u16 count[MAX_BITS + 1];    // codes per length
    u16 symbol[288];            // symbols ordered by code
};

// Returns the decoded symbol or -1.
static int decode(Inflate* s, const Huffman* h) {
    int code = 0, first = 0, index = 0;
    for (u32 len = 1; len <= MAX_BITS; ++len) {
        code |= (int)get_bits(s, 1);
        if (s->err) return -1;
        const int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// Builds the decoding tables from code lengths. Returns < 0 for an over-subscribed set, 0 when complete,
// > 0 when incomplete (allowed; a code that isn't there just fails to decode).
static int build(Huffman* h, const u16* length, u32 n) {
    memset(h->count, 0, sizeof(h->count));
    for (u32 i = 0; i < n; ++i) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (u32 len = 1; len <= MAX_BITS; ++len) {
        left = (left << 1) - h->count[len];
        if (left < 0) return left;
    }
    u16 offs[MAX_BITS + 1];
    offs[1] = 0;
    for (u32 len = 1; len < MAX_BITS; ++len) offs[len + 1] = (u16)(offs[len] + h->count[len]);
    for (u32 i = 0; i < n; ++i)
        if (length[i]) h->symbol[offs[length[i]]++] = (u16)i;
    return left;
}

static const u16 LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                  115, 131, 163, 195, 227, 258 };
static const u8 LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                   12, 12, 13, 13 };

static bool codes(Inflate* s, const Huffman* lit, const Huffman* dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s->out_pos >= s->out_size) return false;
            s->out[s->out_pos++] = (u8)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const u32 len = LEN_BASE[sym] + get_bits(s, LEN_EXTRA[sym]);
            const int ds = decode(s, dist);
            if (ds < 0 || ds >= 30) return false;
            const u32 d = DIST_BASE[ds] + get_bits(s, DIST_EXTRA[ds]);
            if (s->err || d > s->out_pos || len > s->out_size - s->out_pos) return false;
            for (u32 i = 0; i < len; ++i, ++s->out_pos) s->out[s->out_pos] = s->out[s->out_pos - d];
        }
    }
}

static bool block_stored(Inflate* s) {
    s->bitbuf = s->bitcnt = 0;      // to the next byte boundary
    if (s->in_size - s->in_pos < 4) return false;
    const u8* p = s->in + s->in_pos;
    const u32 len = p[0] | (u32)p[1] << 8, nlen = p[2] | (u32)p[3] << 8;
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF) || len > s->in_size - s->in_pos || len > s->out_size - s->out_pos) return false;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return true;
}

static bool block_fixed(Inflate* s) {
    u16 len[288];
    u32 i = 0;
    for (; i < 144; ++i) len[i] = 8;
    for (; i < 256; ++i) len[i] = 9;
    for (; i < 280; ++i) len[i] = 7;
    for (; i < 288; ++i) len[i] = 8;
    Huffman lit, dist;
    build(&lit, len, 288);
    for (i = 0; i < 30; ++i) len[i] = 5;
    build(&dist, len, 30);
    return codes(s, &lit, &dist);
}

static bool block_dynamic(Inflate* s) {
    static const u8 ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    const u32 nlen = get_bits(s, 5) + 257, ndist = get_bits(s, 5) + 1, ncode = get_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return false;
    u16 len[286 + 30] = {};
    for (u32 i = 0; i < ncode; ++i) len[ORDER[i]] = (u16)get_bits(s, 3);
    Huffman lencode, lit, dist;
    if (s->err || build(&lencode, len, 19) != 0) return false;      // the code-length code must be complete
    for (u32 i = 0; i < nlen + ndist;) {
        const int sym = decode(s, &lencode);
        if (sym < 0) return false;
        if (sym < 16) {
            len[i++] = (u16)sym;
            continue;
        }
        u16 v = 0;
        u32 rep;
        if (sym == 16) {
            if (!i) return false;
            v = len[i - 1];
            rep = 3 + get_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + get_bits(s, 3);
        } else {
            rep = 11 + get_bits(s, 7);
        }
        if (s->err || i + rep > nlen + ndist) return false;
        while (rep--) len[i++] = v;
    }
    if (!len[256]) return false;    // no end-of-block code
    if (build(&lit, len, nlen) < 0 || build(&dist, len + nlen, ndist) < 0) return false;
    return codes(s, &lit, &dist);
}

static bool inflate(const u8* in, u32 in_size, u8* out, u32 out_size) {
    Inflate s = { in, in_size, 0, 0, 0, false, out, out_size, 0 };
    u32 last;
    do {
        last = get_bits(&s, 1);
        const u32 type = get_bits(&s, 2);
        if (s.err) return false;
        const bool ok = type == 0 ? block_stored(&s) : type == 1 ? block_fixed(&s) : type == 2 && block_dynamic(&s);
        if (!ok) return false;
    } while (!last);
    return s.out_pos == out_size;
}

// ---- archive ------------------------------------------------------------------------------------------------

static u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8* p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

constexpr u32 SIG_EOCD = 0x06054B50, SIG_CENTRAL = 0x02014B50, SIG_LOCAL = 0x04034B50;

bool zip_find(const u8* zip, u32 zip_size, const char* name, ZipEntry* e) {
    if (zip_size < 22) return false;
    // End of central directory: the last 22 bytes plus up to 64 KB of comment.
    u32 eocd = zip_size - 22;
    const u32 lowest = zip_size > 22 + 0xFFFF ? zip_size - 22 - 0xFFFF : 0;
    while (rd32(zip + eocd) != SIG_EOCD) {
        if (eocd == lowest) return false;
        --eocd;
    }
    const u32 entries = rd16(zip + eocd + 10), cd_size = rd32(zip + eocd + 12), cd_off = rd32(zip + eocd + 16);
    if (cd_off > eocd || cd_size > eocd - cd_off) return false;
    const size_t name_len = strlen(name);
    u32 p = cd_off;
    for (u32 i = 0; i < entries; ++i) {
        if (cd_off + cd_size - p < 46 || rd32(zip + p) != SIG_CENTRAL) return false;
        const u8* c = zip + p;
        const u32 n = rd16(c + 28), extra = rd16(c + 30), comment = rd16(c + 32);
        if (cd_off + cd_size - p - 46 < n + extra + comment) return false;
        if (n == name_len && !memcmp(c + 46, name, n)) {
            if (rd16(c + 8) & 1) return false;     // encrypted
            const u32 local = rd32(c + 42);
            if (local > cd_off || cd_off - local < 30 || rd32(zip + local) != SIG_LOCAL) return false;
            const u32 data = local + 30 + rd16(zip + local + 26) + rd16(zip + local + 28);
            e->method = rd16(c + 10);
            e->crc = rd32(c + 16);
            e->comp_size = rd32(c + 20);
            e->size = rd32(c + 24);
            e->data_offset = data;
            return data <= cd_off && e->comp_size <= cd_off - data;
        }
        p += 46 + n + extra + comment;
    }
    return false;
}

bool zip_read(const u8* zip, u32 zip_size, const ZipEntry* e, u8* out) {
    if (e->data_offset > zip_size || e->comp_size > zip_size - e->data_offset) return false;
    const u8* data = zip + e->data_offset;
    bool ok;
    if (e->method == 0) {
        ok = e->comp_size == e->size;
        if (ok) memcpy(out, data, e->size);
    } else {
        ok = e->method == 8 && inflate(data, e->comp_size, out, e->size);
    }
    return ok && crc32_ieee(out, e->size) == e->crc;
}
