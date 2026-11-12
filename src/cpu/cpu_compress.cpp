// Compression workloads: the Windows Compression API (XPRESS, XPRESS-Huffman, MSZIP, LZMS; cabinet.dll,
// part of Windows) and our own LZ77 (LZ4-style byte format, greedy hash matcher) as a pure-integer
// workload. Every test compresses and decompresses a text-like corpus and checks the round trip.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/sysinfo.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <windows.h>
#include <compressapi.h>
#include <immintrin.h>
#include <stdio.h>
#include <string.h>

// ---- corpus --------------------------------------------------------------------------------------------
// Text-like, deterministic: prose from a Zipf-ish vocabulary, log lines, JSON records and a few numeric
// tables. Compresses about like real text and markup (XPRESS ~2.5-3x, LZMS ~4x).

static const char* const WORDS[] = {
    "the", "of", "and", "to", "in", "a", "is", "that", "for", "it", "as", "was", "with", "be", "by", "on", "not",
    "he", "this", "are", "or", "his", "from", "at", "which", "but", "have", "an", "had", "they", "you", "were",
    "their", "one", "all", "we", "can", "her", "has", "there", "been", "if", "more", "when", "will", "would",
    "who", "so", "no", "system", "memory", "process", "thread", "value", "result", "number", "between", "during",
    "benchmark", "performance", "compression", "however", "because", "another", "through", "network", "storage",
    "temperature", "frequency", "processor", "graphics", "measure", "window", "document", "question", "important",
};

static u64 corpus_rng(u64* s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static u64 put(u8* dst, u64 at, u64 cap, const char* s) {
    while (*s && at < cap) dst[at++] = (u8)*s++;
    return at;
}

static u64 put_num(u8* dst, u64 at, u64 cap, u64 v) {
    char tmp[24];
    u32 n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && at < cap) dst[at++] = (u8)tmp[--n];
    return at;
}

void corpus_fill(u8* dst, u64 bytes, u64 seed) {
    u64 s = seed | 1, at = 0;
    const u32 nw = ARRAY_COUNT(WORDS);
    while (at < bytes) {
        const u64 kind = corpus_rng(&s) % 16;
        if (kind < 10) {                    // a sentence
            const u32 len = 6 + (u32)(corpus_rng(&s) % 14);
            for (u32 w = 0; w < len && at < bytes; ++w) {
                const u64 r = corpus_rng(&s);
                const u32 idx = (u32)((r % nw) * ((r >> 20) % nw) / nw);    // skewed towards common words
                at = put(dst, at, bytes, WORDS[idx]);
                at = put(dst, at, bytes, w + 1 < len ? " " : ".\n");
            }
        } else if (kind < 13) {             // a log line
            at = put(dst, at, bytes, "2026-11-11T12:");
            at = put_num(dst, at, bytes, 10 + corpus_rng(&s) % 50);
            at = put(dst, at, bytes, corpus_rng(&s) & 1 ? " INFO  worker[" : " WARN  worker[");
            at = put_num(dst, at, bytes, corpus_rng(&s) % 16);
            at = put(dst, at, bytes, "] request id=");
            at = put_num(dst, at, bytes, corpus_rng(&s) % 1000000);
            at = put(dst, at, bytes, " took ");
            at = put_num(dst, at, bytes, corpus_rng(&s) % 5000);
            at = put(dst, at, bytes, " us\n");
        } else if (kind < 15) {             // a JSON record
            at = put(dst, at, bytes, "{\"id\": ");
            at = put_num(dst, at, bytes, corpus_rng(&s) % 100000);
            at = put(dst, at, bytes, ", \"name\": \"");
            at = put(dst, at, bytes, WORDS[corpus_rng(&s) % nw]);
            at = put(dst, at, bytes, "\", \"score\": ");
            at = put_num(dst, at, bytes, corpus_rng(&s) % 1000);
            at = put(dst, at, bytes, ", \"active\": true}\n");
        } else {                            // a row of numbers
            for (u32 c = 0; c < 8 && at < bytes; ++c) {
                at = put_num(dst, at, bytes, corpus_rng(&s) % 65536);
                at = put(dst, at, bytes, c < 7 ? "\t" : "\n");
            }
        }
    }
}

// ---- own LZ77 ------------------------------------------------------------------------------------------
// LZ4-style sequences: token (literal length << 4 | match length - 4), length bytes for values >= 15,
// literals, 16-bit offset. Greedy matcher with a 4-byte hash table of the last position.

static constexpr u32 LZ_HASH_BITS = 14;

static inline u32 load32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 load64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }
static inline u32 lz_hash(u32 v) { return (v * 2654435761u) >> (32 - LZ_HASH_BITS); }

static u8* lz_put_len(u8* op, u64 len) {
    for (; len >= 255; len -= 255) *op++ = 255;
    *op++ = (u8)len;
    return op;
}

// Compresses n bytes; dst needs n + n / 255 + 16 bytes. `table` holds 1 << LZ_HASH_BITS u32.
FORCE_INLINE static u64 lz77_compress_body(const u8* src, u64 n, u8* dst, u32* table) {
    memset(table, 0, sizeof(u32) << LZ_HASH_BITS);
    const u8* ip = src;
    const u8* anchor = src;
    const u8* const end = src + n;
    u8* op = dst;
    if (n > 16) {
        const u8* const match_start_limit = end - 12;
        const u8* const match_end_limit = end - 8;
        ++ip;
        while (ip < match_start_limit) {
            const u32 seq = load32(ip);
            const u32 h = lz_hash(seq);
            const u8* ref = src + table[h];
            table[h] = (u32)(ip - src);
            if (ip - ref > 65535 || ref >= ip || load32(ref) != seq) { ++ip; continue; }
            const u8* mp = ip + 4;
            const u8* rp = ref + 4;
            while (mp < match_end_limit) {
                const u64 diff = load64(mp) ^ load64(rp);
                if (diff) { mp += _tzcnt_u64(diff) >> 3; break; }
                mp += 8;
                rp += 8;
            }
            if (mp > match_end_limit) mp = match_end_limit;
            const u64 lit = (u64)(ip - anchor), ml = (u64)(mp - ip) - 4;
            u8* token = op++;
            *token = (u8)(((lit >= 15 ? 15 : lit) << 4) | (ml >= 15 ? 15 : ml));
            if (lit >= 15) op = lz_put_len(op, lit - 15);
            memcpy(op, anchor, lit);
            op += lit;
            const u16 off = (u16)(ip - ref);
            memcpy(op, &off, 2);
            op += 2;
            if (ml >= 15) op = lz_put_len(op, ml - 15);
            ip = anchor = mp;
        }
    }
    const u64 lit = (u64)(end - anchor);     // last literals, no match
    *op++ = (u8)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) op = lz_put_len(op, lit - 15);
    memcpy(op, anchor, lit);
    return (u64)(op + lit - dst);
}

// Decompresses into dst (needs 32 bytes of slack past the output). Returns the output size, 0 on bad input.
FORCE_INLINE static u64 lz77_decompress_body(const u8* src, u64 n, u8* dst, u64 cap) {
    const u8* ip = src;
    const u8* const iend = src + n;
    u8* op = dst;
    u8* const oend = dst + cap;
    for (;;) {
        if (ip >= iend) return 0;
        const u32 token = *ip++;
        u64 lit = token >> 4;
        if (lit == 15) { u32 b; do { b = *ip++; lit += b; } while (b == 255 && ip < iend); }
        if (lit > (u64)(iend - ip) || lit > (u64)(oend - op)) return 0;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip == iend) return (u64)(op - dst);
        u16 off;
        memcpy(&off, ip, 2);
        ip += 2;
        u64 ml = token & 15;
        if (ml == 15) { u32 b; do { b = *ip++; ml += b; } while (b == 255 && ip < iend); }
        ml += 4;
        if (off == 0 || off > (u64)(op - dst) || ml > (u64)(oend - op)) return 0;
        const u8* m = op - off;
        if (off >= 8) {
            for (u64 i = 0; i < ml; i += 8) memcpy(op + i, m + i, 8);     // may write up to 7 bytes past: slack
        } else {
            for (u64 i = 0; i < ml; ++i) op[i] = m[i];
        }
        op += ml;
    }
}

// AVX2 + baseline copies (single thread -15 % compress, -11 % decompress without the AVX2 one).
TARGET_AVX2 static u64 lz77_compress_avx2(const u8* src, u64 n, u8* dst, u32* table) { return lz77_compress_body(src, n, dst, table); }
static u64 lz77_compress_base(const u8* src, u64 n, u8* dst, u32* table) { return lz77_compress_body(src, n, dst, table); }
u64 lz77_compress(const u8* src, u64 n, u8* dst, u32* table) {
    return g_sys.cpu.avx2 ? lz77_compress_avx2(src, n, dst, table) : lz77_compress_base(src, n, dst, table);
}
TARGET_AVX2 static u64 lz77_decompress_avx2(const u8* src, u64 n, u8* dst, u64 cap) { return lz77_decompress_body(src, n, dst, cap); }
static u64 lz77_decompress_base(const u8* src, u64 n, u8* dst, u64 cap) { return lz77_decompress_body(src, n, dst, cap); }
u64 lz77_decompress(const u8* src, u64 n, u8* dst, u64 cap) {
    return g_sys.cpu.avx2 ? lz77_decompress_avx2(src, n, dst, cap) : lz77_decompress_base(src, n, dst, cap);
}

// ---- shared test driver ------------------------------------------------------------------------------
// Each thread compresses / decompresses its own CHUNK of the corpus per call (different data per thread).

static constexpr u64 CHUNK = 256 * KiB;
static constexpr u64 CORPUS = 16 * CHUNK;
static constexpr u64 COMP_CAP = CHUNK + CHUNK / 2 + 4096;

struct CompCtx {
    u8* corpus;
    u8* comp;               // per thread: COMP_CAP compressed bytes
    u8* plain;              // per thread: CHUNK + 64 decompressed bytes
    u64 comp_size[MAX_LP];
    u32* tables;            // LZ77 hash tables, per thread
    u32 algorithm;          // COMPRESS_ALGORITHM_* or 0 = own LZ77
    COMPRESSOR_HANDLE comp_h[MAX_LP];
    DECOMPRESSOR_HANDLE dec_h[MAX_LP];
    bool failed;
};

static const u8* chunk_src(const CompCtx* c, u32 index) { return c->corpus + (u64)(index % 16) * CHUNK; }
static u8* comp_buf(const CompCtx* c, u32 index) { return c->comp + (u64)index * COMP_CAP; }
static u8* plain_buf(const CompCtx* c, u32 index) { return c->plain + (u64)index * (CHUNK + 64); }

static u64 do_compress(CompCtx* c, u32 index, u8* dst) {
    if (!c->algorithm) return lz77_compress(chunk_src(c, index), CHUNK, dst, c->tables + ((u64)index << LZ_HASH_BITS));
    SIZE_T size = 0;
    if (!Compress(c->comp_h[index], chunk_src(c, index), CHUNK, dst, COMP_CAP, &size)) return 0;
    return size;
}

static u64 do_decompress(CompCtx* c, u32 index, u8* dst) {
    if (!c->algorithm) return lz77_decompress(comp_buf(c, index), c->comp_size[index], dst, CHUNK);
    SIZE_T size = 0;
    if (!Decompress(c->dec_h[index], comp_buf(c, index), c->comp_size[index], dst, CHUNK, &size)) return 0;
    return size;
}

// Compression is deterministic: recompressing a chunk rewrites the same bytes the decompress phase reads.
static f64 kern_compress(void* ctx, u32 index, u32) {
    CompCtx* c = (CompCtx*)ctx;
    const u64 size = do_compress(c, index, comp_buf(c, index));
    if (!size) c->failed = true;
    else c->comp_size[index] = size;
    return (f64)CHUNK;
}

static f64 kern_decompress(void* ctx, u32 index, u32) {
    CompCtx* c = (CompCtx*)ctx;
    if (do_decompress(c, index, plain_buf(c, index)) != CHUNK) c->failed = true;
    return (f64)CHUNK;
}

static void comp_destroy(CompCtx* c) {
    for (u32 i = 0; i < MAX_LP; ++i) {
        if (c->comp_h[i]) CloseCompressor(c->comp_h[i]);
        if (c->dec_h[i]) CloseDecompressor(c->dec_h[i]);
    }
    big_free(c->corpus);
    big_free(c->comp);
    big_free(c->plain);
    big_free(c->tables);
}

// Allocates, builds the corpus, opens handles for `threads` workers and pre-compresses every chunk
// (verified by a round trip). Returns the compression ratio, 0 on failure.
static f64 comp_setup(CompCtx* c, u32 algorithm, u32 threads, char* err, u32 err_size) {
    c->algorithm = algorithm;
    c->corpus = (u8*)big_alloc(CORPUS);
    c->comp = (u8*)big_alloc((u64)MAX_LP * COMP_CAP);
    c->plain = (u8*)big_alloc((u64)MAX_LP * (CHUNK + 64));
    c->tables = (u32*)big_alloc((u64)MAX_LP * (sizeof(u32) << LZ_HASH_BITS));
    if (!c->corpus || !c->comp || !c->plain || !c->tables) { snprintf(err, err_size, "out of memory"); return 0.0; }
    corpus_fill(c->corpus, CORPUS, 0x9E3779B97F4A7C15ull);
    u64 in = 0, outb = 0;
    for (u32 i = 0; i < threads; ++i) {
        if (algorithm && (!CreateCompressor(algorithm, nullptr, &c->comp_h[i]) ||
                          !CreateDecompressor(algorithm, nullptr, &c->dec_h[i]))) {
            snprintf(err, err_size, "Compression API: algorithm %u unavailable (error %lu)", algorithm, GetLastError());
            return 0.0;
        }
        c->comp_size[i] = do_compress(c, i, comp_buf(c, i));
        if (!c->comp_size[i]) { snprintf(err, err_size, "compression failed"); return 0.0; }
        u8* out = plain_buf(c, i);
        if (do_decompress(c, i, out) != CHUNK || memcmp(out, chunk_src(c, i), CHUNK) != 0) {
            snprintf(err, err_size, "round trip mismatch");
            return 0.0;
        }
        if (i < 16) { in += CHUNK; outb += c->comp_size[i]; }
    }
    return outb ? (f64)in / outb : 0.0;
}

// Compress ST / MT, decompress ST / MT (each phase half the duration), then the ratio.
static void comp_test(u32 algorithm, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    live_phase(live, "Preparing corpus", "");
    const u32 mt = thread_count_for(p->thread_count);
    CompCtx c = {};
    char err[128] = {};
    const f64 ratio = comp_setup(&c, algorithm, mt, err, sizeof(err));
    if (ratio <= 0.0) { comp_destroy(&c); res_fail(out, "%s", err); return; }
    const f64 seconds = p->duration_ms / 1000.0 / 2.0;
    static const char* const PH[] = { "Compress, single thread", "Compress, multi-thread", "Decompress, single thread",
                                      "Decompress, multi-thread" };
    f64 rate[4] = {};
    for (u32 k = 0; k < 4 && !cancel->load(); ++k) {
        live_phase(live, PH[k], "MB/s");
        rate[k] = run_throughput(k < 2 ? kern_compress : kern_decompress, &c, k & 1 ? mt : 1, seconds, 1e6, live, cancel,
                                 0.25f * k, 0.25f * (k + 1));
    }
    // The timed decompressions wrote every thread's plain buffer: check them once more.
    bool ok = !c.failed;
    for (u32 i = 0; i < mt && ok; ++i) ok = memcmp(plain_buf(&c, i), chunk_src(&c, i), CHUNK) == 0;
    comp_destroy(&c);
    res_begin(out, RESULT_VALUES);
    for (u32 k = 0; k < 4; ++k) res_add(out, PH[k], "MB/s", rate[k]);
    res_add(out, "Compression ratio", "x", ratio);
    if (!ok) res_fail(out, "decompressed data mismatch");
}

void cpu_comp_xpress(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    comp_test(COMPRESS_ALGORITHM_XPRESS, p, live, out, cancel);
}
void cpu_comp_xpress_huff(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    comp_test(COMPRESS_ALGORITHM_XPRESS_HUFF, p, live, out, cancel);
}
void cpu_comp_mszip(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    comp_test(COMPRESS_ALGORITHM_MSZIP, p, live, out, cancel);
}
void cpu_comp_lzms(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    comp_test(COMPRESS_ALGORITHM_LZMS, p, live, out, cancel);
}
void cpu_comp_lz77(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    comp_test(0, p, live, out, cancel);
}

// ---- LZ77 compressor as a plain kernel (hybrid test) ------------------------------------------------

void* lz77_ctx_create() {
    CompCtx* c = (CompCtx*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CompCtx));
    char err[128];
    if (c && comp_setup(c, 0, 1, err, sizeof(err)) > 0.0) return c;
    if (c) { comp_destroy(c); HeapFree(GetProcessHeap(), 0, c); }
    return nullptr;
}

f64 kern_lz77_compress(void* ctx, u32 index, u32 count) { return kern_compress(ctx, index, count); }

bool lz77_ctx_verify(void* ctx) {
    CompCtx* c = (CompCtx*)ctx;
    u8* out = plain_buf(c, 0);
    return do_decompress(c, 0, out) == CHUNK && memcmp(out, chunk_src(c, 0), CHUNK) == 0;
}

void lz77_ctx_destroy(void* ctx) {
    comp_destroy((CompCtx*)ctx);
    HeapFree(GetProcessHeap(), 0, ctx);
}
