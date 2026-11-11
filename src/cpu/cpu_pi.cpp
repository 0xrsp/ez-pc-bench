// Pi to PI_DIGITS decimal digits, y-cruncher style: Chudnovsky series by binary splitting, big-number
// multiplication by FP64 FFT (base-1000 digits) above a size threshold, Newton iterations for 1 / T and
// 1 / sqrt(10005), all in base 10^9 so no radix conversion is needed. Heavy on FP64 and memory bandwidth.
// Checks: every FFT product is verified modulo 2^61 - 1 and its rounding error must stay below 0.2; the
// result must start with the known 50 digits and have the six 9s at decimal places 762-767; the single-
// and multi-thread runs must agree on every digit.
// Multi-thread: the series is split into one range per thread, merged pairwise in parallel; the final
// merges and the Newton steps run on one thread.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/throughput.h"
#include "core/timer.h"
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <string.h>

static constexpr u32 PI_DIGITS = 1000000;
static constexpr u32 BASE = 1000000000;            // limb base 10^9
static constexpr u32 SCHOOL_LIMIT = 48;            // schoolbook below this many limbs (smaller operand)

// ---- big integers ---------------------------------------------------------------------------------------
// Little-endian base-10^9 limbs, sign + magnitude, n = 0 for zero. Heap storage, freed explicitly.

struct Big {
    u32* d;
    u32 n, cap;
    bool neg;
};

static void bn_reserve(Big* b, u32 cap) {
    if (cap <= b->cap) return;
    HANDLE heap = GetProcessHeap();
    b->d = (u32*)(b->d ? HeapReAlloc(heap, 0, b->d, cap * sizeof(u32)) : HeapAlloc(heap, 0, cap * sizeof(u32)));
    b->cap = cap;
}

static void bn_free(Big* b) {
    if (b->d) HeapFree(GetProcessHeap(), 0, b->d);
    *b = {};
}

static void bn_trim(Big* b) {
    while (b->n && b->d[b->n - 1] == 0) --b->n;
    if (!b->n) b->neg = false;
}

static void bn_set_u64(Big* b, u64 v) {
    bn_reserve(b, 3);
    b->n = 0;
    b->neg = false;
    while (v) { b->d[b->n++] = (u32)(v % BASE); v /= BASE; }
}

static i32 cmp_abs(const Big* a, const Big* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (u32 i = a->n; i-- > 0;)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

// r = |a| + |b| (r may alias a or b).
static void add_abs(Big* r, const Big* a, const Big* b) {
    if (a->n < b->n) { const Big* t = a; a = b; b = t; }
    const u32 an = a->n, bn = b->n;
    bn_reserve(r, an + 1);
    u32 carry = 0;
    for (u32 i = 0; i < an; ++i) {
        u32 s = a->d[i] + (i < bn ? b->d[i] : 0) + carry;
        carry = s >= BASE;
        r->d[i] = carry ? s - BASE : s;
    }
    r->d[an] = carry;
    r->n = an + 1;
    bn_trim(r);
}

// r = |a| - |b| with |a| >= |b| (r may alias a or b).
static void sub_abs(Big* r, const Big* a, const Big* b) {
    const u32 an = a->n, bn = b->n;
    bn_reserve(r, an);
    i32 borrow = 0;
    for (u32 i = 0; i < an; ++i) {
        i64 s = (i64)a->d[i] - (i < bn ? b->d[i] : 0) - borrow;
        borrow = s < 0;
        r->d[i] = (u32)(borrow ? s + BASE : s);
    }
    r->n = an;
    bn_trim(r);
}

// r = a + b, signed (r may alias).
static void bn_add(Big* r, const Big* a, const Big* b) {
    const bool an = a->neg, bneg = b->neg;
    if (an == bneg) { add_abs(r, a, b); r->neg = an && r->n; return; }
    if (cmp_abs(a, b) >= 0) { sub_abs(r, a, b); r->neg = an && r->n; }
    else { sub_abs(r, b, a); r->neg = bneg && r->n; }
}

static void bn_sub(Big* r, const Big* a, const Big* b) {
    Big nb = *b;            // shallow copy with the sign flipped
    nb.neg = !b->neg && b->n;
    bn_add(r, a, &nb);
}

// r = a * m (m < 2^32; r may alias a).
static void bn_mul_small(Big* r, const Big* a, u32 m) {
    bn_reserve(r, a->n + 2);
    u64 carry = 0;
    for (u32 i = 0; i < a->n; ++i) {
        const u64 t = (u64)a->d[i] * m + carry;
        r->d[i] = (u32)(t % BASE);
        carry = t / BASE;
    }
    u32 n = a->n;
    while (carry) { r->d[n++] = (u32)(carry % BASE); carry /= BASE; }
    r->n = n;
    r->neg = a->neg;
    bn_trim(r);
}

// r = a / m, truncated (r may alias a).
static void bn_div_small(Big* r, const Big* a, u32 m) {
    bn_reserve(r, a->n);
    u64 rem = 0;
    for (u32 i = a->n; i-- > 0;) {
        const u64 cur = rem * BASE + a->d[i];
        r->d[i] = (u32)(cur / m);
        rem = cur % m;
    }
    r->n = a->n;
    r->neg = a->neg;
    bn_trim(r);
}

// r = a * BASE^k (k may be negative: truncating shift right). r must not alias a.
static void bn_shift(Big* r, const Big* a, i32 k) {
    if (k >= 0) {
        bn_reserve(r, a->n + k + 1);
        memset(r->d, 0, k * sizeof(u32));
        memcpy(r->d + k, a->d, a->n * sizeof(u32));
        r->n = a->n ? a->n + k : 0;
    } else {
        const u32 s = (u32)-k;
        r->n = a->n > s ? a->n - s : 0;
        bn_reserve(r, r->n + 1);
        memcpy(r->d, a->d + s, r->n * sizeof(u32));
    }
    r->neg = a->neg && r->n;
}

// ---- multiplication ----------------------------------------------------------------------------------

struct PiThread {
    Cpx* fft;               // 2 * cap complex
    u64 fft_cap;
    u32* digits;            // base-1000 product digits
    u64 digits_cap;
};

struct PiShared {
    Cpx* w;                 // twiddles for w_n points
    u32 w_n;
    std::atomic<u32> bad_products;      // modular check or rounding error failures
    std::atomic<u32> max_err_milli;     // largest FFT rounding error seen, x1000
};
static PiShared s_pi;

static constexpr u64 P61 = (1ull << 61) - 1;

static u64 mulmod61(u64 a, u64 b) {
    u64 hi;
    const u64 lo = _umul128(a, b, &hi);
    u64 r = (lo & P61) + ((lo >> 61) | (hi << 3));
    r = (r & P61) + (r >> 61);
    return r >= P61 ? r - P61 : r;
}

static u64 mod61(const u32* d, u32 n) {
    u64 r = 0;
    for (u32 i = n; i-- > 0;) {
        r = mulmod61(r, BASE) + d[i];
        if (r >= P61) r -= P61;
    }
    return r;
}

static void school_mul(const u32* a, u32 na, const u32* b, u32 nb, u32* out) {
    memset(out, 0, (na + nb) * sizeof(u32));
    for (u32 i = 0; i < na; ++i) {
        u64 carry = 0;
        const u64 ai = a[i];
        for (u32 j = 0; j < nb; ++j) {
            const u64 t = ai * b[j] + out[i + j] + carry;
            out[i + j] = (u32)(t % BASE);
            carry = t / BASE;
        }
        out[i + nb] = (u32)carry;
    }
}

static inline u32 digit3(const u32* a, u32 k) {
    const u32 limb = a[k / 3];
    const u32 r = k % 3;
    return r == 0 ? limb % 1000 : r == 1 ? limb / 1000 % 1000 : limb / 1000000;
}

// out (na + nb limbs) = a * b through a complex FFT of base-1000 digits: a in the real part, b in the
// imaginary part, one forward and one inverse transform.
static void fft_mul(const u32* a, u32 na, const u32* b, u32 nb, u32* out, PiThread* t) {
    const u32 la = na * 3, lb = nb * 3, need = la + lb;
    u32 n = 1;
    while (n < need) n *= 2;
    if ((u64)2 * n > t->fft_cap) {
        big_free(t->fft);
        t->fft_cap = (u64)2 * n;
        t->fft = (Cpx*)big_alloc(t->fft_cap * sizeof(Cpx));
    }
    if (need > t->digits_cap) {
        big_free(t->digits);
        t->digits_cap = need;
        t->digits = (u32*)big_alloc(t->digits_cap * sizeof(u32));
    }
    Cpx* z = t->fft;
    Cpx* y = z + n;
    for (u32 k = 0; k < n; ++k) z[k] = { k < la ? (f64)digit3(a, k) : 0.0, k < lb ? (f64)digit3(b, k) : 0.0 };
    const u32 stride = s_pi.w_n / n;
    Cpx* zf = fft_stockham(z, y, s_pi.w, n, stride);
    Cpx* p = zf == z ? y : z;
    // A*B = (Z[k]^2 - conj(Z[-k])^2) / 4i; conjugated for the inverse transform by conjugation.
    for (u32 k = 0; k < n; ++k) {
        const Cpx x = zf[k], c = { zf[(n - k) & (n - 1)].re, -zf[(n - k) & (n - 1)].im };
        const f64 re = x.re * x.re - x.im * x.im - (c.re * c.re - c.im * c.im);
        const f64 im = 2.0 * x.re * x.im - 2.0 * c.re * c.im;
        p[k] = { im * 0.25, re * 0.25 };    // (re + i im) / 4i = (im - i re) / 4, conjugated -> (im, re) / 4
    }
    Cpx* r = fft_stockham(p, zf, s_pi.w, n, stride);
    const f64 inv = 1.0 / n;
    f64 max_err = 0.0;
    u64 carry = 0;
    for (u32 k = 0; k < need; ++k) {
        const f64 v = r[k].re * inv;
        const f64 rv = nearbyint(v);
        const f64 e = fabs(v - rv);
        if (e > max_err) max_err = e;
        const u64 tot = (u64)(rv < 0.0 ? 0.0 : rv) + carry;
        t->digits[k] = (u32)(tot % 1000);
        carry = tot / 1000;
    }
    for (u32 i = 0; i < na + nb; ++i)
        out[i] = t->digits[3 * i] + t->digits[3 * i + 1] * 1000 + t->digits[3 * i + 2] * 1000000;
    const u32 milli = (u32)(max_err * 1000.0);
    u32 prev = s_pi.max_err_milli.load(std::memory_order_relaxed);
    while (milli > prev && !s_pi.max_err_milli.compare_exchange_weak(prev, milli)) {}
    if (carry || max_err > 0.2 || mulmod61(mod61(a, na), mod61(b, nb)) != mod61(out, na + nb))
        s_pi.bad_products.fetch_add(1, std::memory_order_relaxed);
}

// r = a * b, signed. r must not alias a or b.
static void bn_mul(Big* r, const Big* a, const Big* b, PiThread* t) {
    if (!a->n || !b->n) { r->n = 0; r->neg = false; return; }
    bn_reserve(r, a->n + b->n + 1);
    if ((a->n < b->n ? a->n : b->n) < SCHOOL_LIMIT) school_mul(a->d, a->n, b->d, b->n, r->d);
    else fft_mul(a->d, a->n, b->d, b->n, r->d, t);
    r->n = a->n + b->n;
    r->neg = a->neg != b->neg;
    bn_trim(r);
}

// ---- Chudnovsky binary splitting ------------------------------------------------------------------------
// pi = 426880 sqrt(10005) Q(0, N) / T(0, N) with P(k) = (6k-5)(2k-1)(6k-1), Q(k) = k^3 C^3 / 24,
// T(k) = (-1)^k P(k) (13591409 + 545140134 k).

struct PQT { Big p, q, t; };

static void pqt_free(PQT* x) { bn_free(&x->p); bn_free(&x->q); bn_free(&x->t); }

// Merges left (a, m) and right (m, b) into left. `need_p`: P is only needed below the top.
static void pqt_merge(PQT* l, PQT* r, bool need_p, PiThread* th) {
    Big t1 = {}, t2 = {}, tmp = {};
    bn_mul(&t1, &r->q, &l->t, th);      // Q2 T1
    bn_mul(&t2, &l->p, &r->t, th);      // P1 T2
    bn_add(&l->t, &t1, &t2);
    bn_mul(&tmp, &l->q, &r->q, th);
    bn_free(&l->q);
    l->q = tmp;
    tmp = {};
    if (need_p) {
        bn_mul(&tmp, &l->p, &r->p, th);
        bn_free(&l->p);
        l->p = tmp;
    }
    bn_free(&t1);
    bn_free(&t2);
    pqt_free(r);
}

static void bs(u64 a, u64 b, PQT* out, bool need_p, PiThread* th) {
    if (b - a == 1) {
        if (a == 0) {
            bn_set_u64(&out->p, 1);
            bn_set_u64(&out->q, 1);
        } else {
            bn_set_u64(&out->p, (6 * a - 5) * (2 * a - 1) * (6 * a - 1));
            Big a3 = {}, c3 = {};
            bn_set_u64(&a3, a * a * a);
            bn_set_u64(&c3, 10939058860032000ull);     // 640320^3 / 24
            bn_mul(&out->q, &a3, &c3, th);
            bn_free(&a3);
            bn_free(&c3);
        }
        Big lin = {};
        bn_set_u64(&lin, 13591409ull + 545140134ull * a);
        bn_mul(&out->t, &out->p, &lin, th);
        out->t.neg = (a & 1) && out->t.n;
        bn_free(&lin);
        return;
    }
    const u64 m = (a + b) / 2;
    PQT r = {};
    bs(a, m, out, true, th);
    bs(m, b, &r, true, th);
    pqt_merge(out, &r, need_p, th);
}

// ---- fixed point and Newton -----------------------------------------------------------------------------
// A fixed-point value with `prec` fractional limbs is a Big M meaning M / BASE^prec.

static void fx_mul(Big* r, const Big* a, const Big* b, u32 prec, PiThread* th) {
    Big full = {};
    bn_mul(&full, a, b, th);
    bn_shift(r, &full, -(i32)prec);
    bn_free(&full);
}

// Top `prec` limbs of x as a fraction in [1/BASE, 1): returns x scaled so x = frac * BASE^(x->n).
static void fx_top(Big* r, const Big* x, u32 prec) { bn_shift(r, x, (i32)prec - (i32)x->n); }

static void fx_one(Big* r, u32 prec) {
    bn_reserve(r, prec + 1);
    memset(r->d, 0, prec * sizeof(u32));
    r->d[prec] = 1;
    r->n = prec + 1;
    r->neg = false;
}

// Precision ladder for Newton: final, then halving down to ~2 limbs.
static u32 newton_ladder(u32 final_prec, u32* steps) {
    u32 n = 0, p = final_prec;
    while (p > 3 && n < 40) { steps[n++] = p; p = p / 2 + 1; }
    for (u32 i = 0; i < n / 2; ++i) { u32 t = steps[i]; steps[i] = steps[n - 1 - i]; steps[n - 1 - i] = t; }
    return n;
}

// y = 1 / f with f = top limbs of x (x as a fraction in [1/BASE, 1)), `prec` fractional limbs.
static void fx_reciprocal(Big* y, const Big* x, u32 prec, PiThread* th) {
    u32 steps[40];
    const u32 n = newton_ladder(prec, steps);
    // Start: ~16 digits from doubles (top two limbs), as a fixed-point value with 2 fractional limbs.
    const f64 top = ((f64)x->d[x->n - 1] + (x->n >= 2 ? x->d[x->n - 2] / (f64)BASE : 0.0)) / BASE;
    const f64 inv = 1.0 / top;      // in (1, BASE]
    const f64 hi = floor(inv);
    u32 cur = 2;
    bn_reserve(y, 4);
    y->d[0] = 0;
    y->d[1] = (u32)floor((inv - hi) * BASE);
    y->d[2] = (u32)fmod(hi, (f64)BASE);
    y->d[3] = (u32)(hi / BASE);
    y->n = 4;
    y->neg = false;
    bn_trim(y);
    for (u32 s = 0; s < n; ++s) {
        const u32 p = steps[s];
        Big ext = {}, f = {}, fy = {}, one = {}, e = {}, ye = {};
        bn_shift(&ext, y, (i32)p - (i32)cur);       // y at the new precision
        bn_free(y);
        *y = ext;
        cur = p;
        fx_top(&f, x, p);
        fx_mul(&fy, &f, y, p, th);
        fx_one(&one, p);
        bn_sub(&e, &one, &fy);                      // e = 1 - f y
        fx_mul(&ye, y, &e, p, th);
        bn_add(y, y, &ye);                          // y += y e
        bn_free(&f); bn_free(&fy); bn_free(&one); bn_free(&e); bn_free(&ye);
    }
    if (cur != prec) {
        Big ext = {};
        bn_shift(&ext, y, (i32)prec - (i32)cur);
        bn_free(y);
        *y = ext;
    }
}

// z = 1 / sqrt(v) for a small integer v, `prec` fractional limbs: z += z (1 - v z^2) / 2.
static void fx_invsqrt_small(Big* z, u32 v, u32 prec, PiThread* th) {
    u32 steps[40];
    const u32 n = newton_ladder(prec, steps);
    const f64 start = 1.0 / sqrt((f64)v);
    u32 cur = 2;
    bn_reserve(z, 3);
    z->d[0] = (u32)(fmod(start * BASE * (f64)BASE, (f64)BASE));
    z->d[1] = (u32)(start * BASE);
    z->d[2] = 0;
    z->n = 3;
    z->neg = false;
    bn_trim(z);
    for (u32 s = 0; s < n; ++s) {
        const u32 p = steps[s];
        Big ext = {}, z2 = {}, vz2 = {}, one = {}, e = {}, ze = {};
        bn_shift(&ext, z, (i32)p - (i32)cur);
        bn_free(z);
        *z = ext;
        cur = p;
        fx_mul(&z2, z, z, p, th);
        bn_mul_small(&vz2, &z2, v);
        fx_one(&one, p);
        bn_sub(&e, &one, &vz2);
        fx_mul(&ze, z, &e, p, th);
        bn_div_small(&ze, &ze, 2);
        bn_add(z, z, &ze);
        bn_free(&z2); bn_free(&vz2); bn_free(&one); bn_free(&e); bn_free(&ze);
    }
}

// pi with `prec` fractional limbs from the series sums Q and T.
static void pi_finish(Big* pi, const Big* q, const Big* t, u32 prec, PiThread* th) {
    // Q / T = (Qtop * B^(q.n - prec)) * (1 / Ttop) * B^(-t.n) with Qtop, Ttop as fractions of prec limbs:
    // Q / T = qf * B^(q.n) / (tf * B^(t.n)) where qf, tf are the fractions.
    Big y = {}, qf = {}, qt = {}, z = {}, s = {};
    fx_reciprocal(&y, t, prec, th);                 // 1 / tf
    fx_top(&qf, q, prec);
    fx_mul(&qt, &qf, &y, prec, th);                 // qf / tf
    // Shift by B^(q.n - t.n).
    Big tmp = {};
    bn_shift(&tmp, &qt, (i32)q->n - (i32)t->n);
    bn_free(&qt);
    fx_invsqrt_small(&z, 10005, prec, th);
    fx_mul(&s, &tmp, &z, prec, th);                 // Q / (T sqrt(10005))
    bn_mul_small(&s, &s, 10005);                    // * sqrt(10005)^2 -> Q sqrt(10005) / T
    bn_mul_small(pi, &s, 426880);
    bn_free(&y); bn_free(&qf); bn_free(&tmp); bn_free(&z); bn_free(&s);
}

// Decimal digit `j` (1-based, after the point) of a fixed-point value with `prec` fractional limbs.
static u32 frac_digit(const Big* x, u32 prec, u32 j) {
    const u32 limb_i = prec - 1 - (j - 1) / 9;
    if (limb_i >= x->n) return 0;
    u32 v = x->d[limb_i];
    for (u32 k = (j - 1) % 9; k < 8; ++k) v /= 10;
    return v % 10;
}

// ---- test ------------------------------------------------------------------------------------------------

struct PiRun {
    u64 terms;
    u32 parts;
    u64 lo[MAX_LP + 1];
    PQT part[MAX_LP];
    PiThread th[MAX_LP];
    u32 pair_count, stride;     // merge level: part[i * 2 * stride] += part[i * 2 * stride + stride]
};
static PiRun s_run;

static void part_task(u32 index, u32, void* user) {
    PiRun* r = (PiRun*)user;
    bs(r->lo[index], r->lo[index + 1], &r->part[index], true, &r->th[index]);
}

static void merge_task(u32 index, u32, void* user) {
    PiRun* r = (PiRun*)user;
    const u32 a = index * 2 * r->stride, b = a + r->stride;
    const bool top = r->stride * 2 >= r->parts;
    pqt_merge(&r->part[a], &r->part[b], !top, &r->th[index]);
}

// Computes pi with `threads` series ranges; returns seconds, digits in *pi (prec fractional limbs).
static f64 pi_compute(u32 threads, Big* pi, u32 prec) {
    PiRun* r = &s_run;
    r->parts = threads;
    for (u32 i = 0; i <= threads; ++i) r->lo[i] = r->terms * i / threads;
    const f64 t0 = timer_now();
    if (threads == 1) bs(0, r->terms, &r->part[0], false, &r->th[0]);
    else pool_run(threads, part_task, r);
    for (r->stride = 1; r->stride < threads; r->stride *= 2) {
        r->pair_count = (threads + 2 * r->stride - 1) / (2 * r->stride);
        u32 real_pairs = 0;     // pairs with a right half (odd part counts leave the last one alone)
        for (u32 i = 0; i < r->pair_count; ++i) real_pairs += i * 2 * r->stride + r->stride < threads;
        pool_run(real_pairs, merge_task, r);
    }
    pi_finish(pi, &r->part[0].q, &r->part[0].t, prec, &r->th[0]);
    const f64 dt = timer_now() - t0;
    pqt_free(&r->part[0]);
    return dt;
}

static const char PI_50[] = "14159265358979323846264338327950288419716939937510";

static bool pi_check(const Big* pi, u32 prec) {
    if (pi->n != prec + 1 || pi->d[prec] != 3) return false;
    for (u32 j = 1; j <= 50; ++j)
        if (frac_digit(pi, prec, j) != (u32)(PI_50[j - 1] - '0')) return false;
    for (u32 j = 762; j <= 767; ++j)            // the Feynman point
        if (frac_digit(pi, prec, j) != 9) return false;
    return true;
}

void cpu_pi(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u32 prec = PI_DIGITS / 9 + 3;         // 3 guard limbs
    PiRun* r = &s_run;
    r->terms = (u64)(PI_DIGITS / 14.181647462725477) + 2;
    // Twiddles for the largest transform: Q and T reach ~2x the digit count, base 1000, product doubles it.
    u32 wn = 1;
    while (wn < 2u * 3 * (2 * prec + 64)) wn *= 2;
    s_pi.w = (Cpx*)big_alloc((u64)wn * sizeof(Cpx));
    if (!s_pi.w) { res_fail(out, "out of memory"); return; }
    s_pi.w_n = wn;
    fft_twiddles(s_pi.w, wn);
    s_pi.bad_products.store(0);
    s_pi.max_err_milli.store(0);

    const u32 mt = thread_count_for(p->thread_count);
    u32 parts = 1;
    while (parts * 2 <= mt) parts *= 2;
    Big pi_st = {}, pi_mt = {};
    live_phase(live, "1M digits, single thread", "s");
    const f64 t_st = pi_compute(1, &pi_st, prec);
    live_sample(live, t_st);
    live_progress(live, 0.5f);
    f64 t_mt = 0.0;
    if (!cancel->load()) {
        live_phase(live, "1M digits, multi-thread", "s");
        t_mt = pi_compute(parts, &pi_mt, prec);
        live_sample(live, t_mt);
        live_progress(live, 1.0f);
    }
    const bool same = cancel->load() || (pi_st.n == pi_mt.n && !memcmp(pi_st.d, pi_mt.d, pi_st.n * sizeof(u32)));
    const bool ok = pi_check(&pi_st, prec);
    const u32 bad = s_pi.bad_products.load();
    bn_free(&pi_st);
    bn_free(&pi_mt);
    for (u32 i = 0; i < MAX_LP; ++i) {
        big_free(r->th[i].fft);
        big_free(r->th[i].digits);
        r->th[i] = {};
    }
    big_free(s_pi.w);
    s_pi.w = nullptr;

    res_begin(out, RESULT_VALUES);
    res_add(out, "1M digits, single thread", "s", t_st);
    res_add(out, "1M digits, multi-thread", "s", t_mt);
    res_add(out, "MT / ST", "x", t_mt > 0.0 ? t_st / t_mt : 0.0);
    if (bad) res_fail(out, "%u FFT products failed the modular check (max rounding error %.3f)", bad,
                      s_pi.max_err_milli.load() / 1000.0);
    else if (!ok) res_fail(out, "digits don't match the known digits of pi");
    else if (!same) res_fail(out, "single- and multi-thread results differ");
}
