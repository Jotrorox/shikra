/* rt_big.c -- optional bignums (spec §3.2 "behind a flag": --bignum; DESIGN.md "Bignums as built").
 * Pasted into a program only when it is built with --bignum (RT_BIGNUM); always part of the tool,
 * where rt_bignum_on switches promotion on. A bignum is an immutable raw object (RT_T_BIGNUM): sign
 * in the header flags (RT_F_NEG), magnitude in 32-bit limbs, little-endian, top limb nonzero,
 * length = 4 * limbs. Invariant: every result that fits in a fixnum is a fixnum (big_fin), so a
 * bignum is never equal to a fixnum and eqv?/= on fixnums stay pointer comparisons. Plain C99 with
 * 64-bit intermediates only (no 128-bit type, no libc). Algorithms (n = limbs): schoolbook add and
 * subtract O(n), multiply O(n*m), Knuth's algorithm D for division O(n*m) with a one-limb fast
 * path, decimal printing by repeated division by 10^9 and parsing by multiply-add in chunks (both
 * O(n^2)), Newton's integer square root from 2^ceil(bits/2) (O(log bits) divisions), conversion
 * to double rounded to nearest even from the leading 64 bits plus a sticky bit. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif
#ifdef RT_HAVE_BIG

typedef uint32_t big_l;
typedef struct { const big_l *d; intptr_t n; int neg; big_l t[2]; } big_v; /* a view of an exact integer */
#ifndef SHIKRA_PROGRAM
RT_VAR int rt_bignum_on;
#endif

static void big_view(obj x, big_v *v) {
    if (rt_is_fix(x)) {
        intptr_t i = RT_FIXVAL(x);
        uint64_t u = i < 0 ? 0 - (uint64_t)i : (uint64_t)i;
        v->neg = i < 0;
        v->t[0] = (big_l)u;
        v->t[1] = (big_l)(u >> 32);
        v->n = v->t[1] ? 2 : v->t[0] ? 1 : 0;
        v->d = v->t;
    } else {
        v->d = RT_PTR(x, rt_bignum)->d;
        v->n = RT_HLEN(x) >> 2;
        v->neg = (RT_HFLAGS(x) & RT_F_NEG) != 0;
    }
}
static size_t big_bytes(intptr_t n) { return (sizeof(uintptr_t) + 4 * (size_t)n + 7) & ~(size_t)7; }
static rt_bignum *big_new(intptr_t n) { /* n zero limbs (n >= 1) */
    rt_bignum *p = (rt_bignum *)rt_alloc(RT_HDR(RT_T_BIGNUM, 0, 4 * n), big_bytes(n));
    rt_memset(p->d, 0, 4 * (size_t)n);
    return p;
}
/* The finished value of p's first n limbs: a fixnum if it fits, else p shrunk to its significant
 * limbs (the freed tail becomes a raw filler object, so the collector's walk over headers works). */
static obj big_fin(rt_bignum *p, intptr_t n, int neg) {
    intptr_t m = n;
    size_t o = big_bytes(n), s;
    while (m && !p->d[m - 1]) m--;
    if (m <= 2) {
        uint64_t u = m == 2 ? (uint64_t)p->d[1] << 32 | p->d[0] : m ? p->d[0] : 0;
        if (u <= (uint64_t)RT_FIX_MAX + (uint64_t)neg) return RT_FIX(neg ? (intptr_t)(0 - u) : (intptr_t)u);
    }
    s = big_bytes(m);
    if (o > s) *(uintptr_t *)((char *)p + s) = RT_HDR(RT_T_RAW, 0, o - s - sizeof(uintptr_t));
    p->hdr = RT_HDR(RT_T_BIGNUM, neg ? RT_F_NEG : 0, 4 * m);
    return RT_TAGPTR(p);
}
static int big_clz(big_l x) {
    int n = 0;
    if (!(x >> 16)) { n += 16; x <<= 16; }
    if (!(x >> 24)) { n += 8; x <<= 8; }
    if (!(x >> 28)) { n += 4; x <<= 4; }
    if (!(x >> 30)) { n += 2; x <<= 2; }
    return n + !(x >> 31);
}
static intptr_t big_bits(const big_l *d, intptr_t n) { return n ? 32 * n - big_clz(d[n - 1]) : 0; }

/* ---- magnitudes ---- */
static int mag_cmp(const big_l *a, intptr_t na, const big_l *b, intptr_t nb) {
    if (na != nb) return na < nb ? -1 : 1;
    while (na--) if (a[na] != b[na]) return a[na] < b[na] ? -1 : 1;
    return 0;
}
static void mag_add(big_l *r, const big_l *a, intptr_t na, const big_l *b, intptr_t nb) { /* na >= nb; r: na + 1 */
    uint64_t c = 0;
    intptr_t i;
    for (i = 0; i < na; i++) { c += (uint64_t)a[i] + (i < nb ? b[i] : 0); r[i] = (big_l)c; c >>= 32; }
    r[na] = (big_l)c;
}
static void mag_sub(big_l *r, const big_l *a, intptr_t na, const big_l *b, intptr_t nb) { /* a >= b; r may be a */
    uint64_t br = 0, t;
    intptr_t i;
    for (i = 0; i < na; i++) { t = (uint64_t)a[i] - (i < nb ? b[i] : 0) - br; r[i] = (big_l)t; br = (t >> 32) & 1; }
}
static void mag_mul(big_l *r, const big_l *a, intptr_t na, const big_l *b, intptr_t nb) { /* r zeroed: na + nb */
    intptr_t i, j;
    for (i = 0; i < na; i++) {
        uint64_t c = 0, ai = a[i];
        if (!ai) continue;
        for (j = 0; j < nb; j++) { c += ai * b[j] + r[i + j]; r[i + j] = (big_l)c; c >>= 32; }
        r[i + nb] = (big_l)c;
    }
}
static big_l mag_div1(big_l *q, const big_l *a, intptr_t n, big_l d) { /* q = a / d (q may be a); the remainder */
    uint64_t r = 0;
    while (n--) { r = r << 32 | a[n]; q[n] = (big_l)(r / d); r %= d; }
    return (big_l)r;
}
static void mag_shl(big_l *r, const big_l *a, intptr_t n, int s) { /* r[0..n] = a << s, 0 <= s < 32 (r may be a) */
    big_l hi = 0;
    intptr_t i;
    for (i = n; i-- > 0;) { big_l x = a[i]; r[i + 1] = hi | (s ? x >> (32 - s) : 0); hi = x << s; }
    r[0] = hi;
}
/* Knuth's algorithm D (TAOCP 4.3.1): u (nu + 1 limbs, u[nu] = 0) / v (n >= 2 limbs, top bit of v[n-1]
 * set, u shifted alike): quotient in q[0..nu-n], remainder (still shifted) in u[0..n-1]. */
static void mag_divn(big_l *q, big_l *u, intptr_t nu, const big_l *v, intptr_t n) {
    intptr_t i, j;
    for (j = nu - n; j >= 0; j--) {
        uint64_t num = (uint64_t)u[j + n] << 32 | u[j + n - 1], qh = num / v[n - 1], rh = num % v[n - 1], c = 0, br = 0, t;
        while (qh >> 32 || qh * v[n - 2] > (rh << 32 | u[j + n - 2])) {
            qh--;
            if ((rh += v[n - 1]) >> 32) break;
        }
        for (i = 0; i < n; i++) {
            uint64_t p = qh * v[i] + c;
            c = p >> 32;
            t = (uint64_t)u[i + j] - (big_l)p - br;
            u[i + j] = (big_l)t;
            br = (t >> 32) & 1;
        }
        t = (uint64_t)u[j + n] - c - br;
        u[j + n] = (big_l)t;
        if (t >> 63) { /* qh was one too large: add v back */
            qh--;
            for (c = 0, i = 0; i < n; i++) { c += (uint64_t)u[i + j] + v[i]; u[i + j] = (big_l)c; c >>= 32; }
            u[j + n] += (big_l)c;
        }
        q[j] = (big_l)qh;
    }
}
/* Truncating division of views: *q and *r as finished values (either may be NULL). */
static void big_divmod(const big_v *a, const big_v *b, obj *q, obj *r) {
    intptr_t n = b->n, m = a->n;
    rt_bignum *qq, *u, *v;
    int s;
    if (mag_cmp(a->d, m, b->d, n) < 0) { /* |a| < |b| */
        if (q) *q = RT_FIX(0);
        if (r) { rt_bignum *c = big_new(m + 1); rt_memcpy(c->d, a->d, 4 * (size_t)m); *r = big_fin(c, m, a->neg); }
        return;
    }
    qq = big_new(m);
    if (n == 1) {
        big_l rem = mag_div1(qq->d, a->d, m, b->d[0]);
        if (r) *r = RT_FIX(a->neg ? -(intptr_t)rem : (intptr_t)rem);
        if (q) *q = big_fin(qq, m, a->neg != b->neg);
        return;
    }
    s = big_clz(b->d[n - 1]);
    u = big_new(m + 1);
    v = big_new(n + 1);
    mag_shl(u->d, a->d, m, s);
    mag_shl(v->d, b->d, n, s);
    mag_divn(qq->d, u->d, m, v->d, n);
    if (q) *q = big_fin(qq, m - n + 1, a->neg != b->neg);
    if (r) {
        intptr_t i;
        for (i = 0; i < n; i++) u->d[i] = (u->d[i] >> s) | (s && i + 1 < n ? u->d[i + 1] << (32 - s) : 0);
        *r = big_fin(u, n, a->neg);
    }
}

/* ---- arithmetic on exact integers (the slow paths of rt_num.c) ---- */
static obj big_addsub(obj a, obj b, int sub) {
    big_v x, y, *p = &x, *q = &y, *t;
    rt_bignum *r;
    int c;
    big_view(a, &x);
    big_view(b, &y);
    if (sub) y.neg = !y.neg;
    if (p->n < q->n) { t = p; p = q; q = t; }
    if (p->neg == q->neg) {
        r = big_new(p->n + 1);
        mag_add(r->d, p->d, p->n, q->d, q->n);
        return big_fin(r, p->n + 1, p->neg);
    }
    if ((c = mag_cmp(p->d, p->n, q->d, q->n)) == 0) return RT_FIX(0);
    if (c < 0) { t = p; p = q; q = t; }
    r = big_new(p->n);
    mag_sub(r->d, p->d, p->n, q->d, q->n);
    return big_fin(r, p->n, p->neg);
}
RT_FN obj rt_big_add(obj a, obj b) { return big_addsub(a, b, 0); }
RT_FN obj rt_big_sub(obj a, obj b) { return big_addsub(a, b, 1); }
RT_FN obj rt_big_mul(obj a, obj b) {
    big_v x, y;
    rt_bignum *r;
    big_view(a, &x);
    big_view(b, &y);
    if (!x.n || !y.n) return RT_FIX(0);
    r = big_new(x.n + y.n);
    mag_mul(r->d, x.d, x.n, y.d, y.n);
    return big_fin(r, x.n + y.n, x.neg != y.neg);
}
RT_FN obj rt_big_neg(obj x) { return big_addsub(RT_FIX(0), x, 1); }
RT_FN obj rt_big_fromi(intptr_t v) {
    rt_bignum *r = big_new(2);
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    r->d[0] = (big_l)u;
    r->d[1] = (big_l)(u >> 32);
    return big_fin(r, 2, v < 0);
}
RT_FN int rt_big_cmp(obj a, obj b) {
    big_v x, y;
    int c;
    big_view(a, &x);
    big_view(b, &y);
    if (x.neg != y.neg) return x.neg ? -1 : 1;
    c = mag_cmp(x.d, x.n, y.d, y.n);
    return x.neg ? -c : c;
}

/* ---- doubles ---- */
static double big_dbits(uint64_t u) { union { uint64_t u; double d; } x; x.u = u; return x.d; }
static uint64_t big_ubits(double d) { union { uint64_t u; double d; } x; x.d = d; return x.u; }
/* m (top bit set) * 2^(e-63), plus a sticky bit for anything below m, rounded to nearest even;
 * subnormal results and overflow to infinity included. */
static double big_round(uint64_t m, int sticky, intptr_t e, int neg) {
    intptr_t keep = e < -1022 ? e + 1075 : 53; /* significant bits the result can hold */
    uint64_t q, rest, bits;
    if (e > 1023) bits = 0x7FF0000000000000u;
    else if (keep <= 0) bits = keep == 0 && (m > 0x8000000000000000u || (m == 0x8000000000000000u && sticky));
    else {
        q = m >> (64 - keep);
        rest = m << keep;
        if (rest > 0x8000000000000000u || (rest == 0x8000000000000000u && (sticky || (q & 1)))) q++;
        bits = e < -1022 ? q : ((uint64_t)(e + 1023) << 52) + (q - ((uint64_t)1 << 52)); /* a carry bumps the exponent */
        if (bits > 0x7FF0000000000000u) bits = 0x7FF0000000000000u;
    }
    return big_dbits(bits | (uint64_t)neg << 63);
}
/* The magnitude d[0..n) times 2^-sh, rounded (sticky: nonzero bits below d). */
static double big_dbl(const big_l *d, intptr_t n, int neg, intptr_t sh, int sticky) {
    intptr_t nb, i;
    int z;
    uint64_t hi, lo;
    while (n && !d[n - 1]) n--;
    if (!n) return 0.0;
    nb = big_bits(d, n);
    z = big_clz(d[n - 1]);
    hi = (uint64_t)d[n - 1] << 32 | (n > 1 ? d[n - 2] : 0);
    lo = n > 2 ? d[n - 3] : 0;
    hi = (hi << z) | (z ? lo >> (32 - z) : 0);
    sticky |= (big_l)(lo << z) != 0;
    for (i = 0; i + 3 < n && !sticky; i++) sticky = d[i] != 0;
    return big_round(hi, sticky, nb - 1 - sh, neg);
}
RT_FN double rt_big_to_double(obj x) {
    big_v v;
    big_view(x, &v);
    return big_dbl(v.d, v.n, v.neg, 0, 0);
}
RT_FN obj rt_big_from_double(double d) {
    uint64_t u = big_ubits(d), m = (u & 0xFFFFFFFFFFFFFu) | (uint64_t)1 << 52, hi;
    int be = (int)(u >> 52) & 0x7FF, e = be - 1075, b;
    intptr_t w;
    rt_bignum *r;
    if (e <= 0) return RT_FIX(be < 1023 ? 0 : (intptr_t)(u >> 63 ? -(int64_t)(m >> -e) : (int64_t)(m >> -e)));
    w = e / 32, b = e % 32;
    r = big_new(w + 3);
    hi = b ? m >> (64 - b) : 0;
    m <<= b;
    r->d[w] = (big_l)m;
    r->d[w + 1] = (big_l)(m >> 32);
    r->d[w + 2] = (big_l)hi;
    return big_fin(r, w + 3, (int)(u >> 63));
}
RT_FN int rt_big_cmpd(obj x, double d) {
    double t;
    int c;
    if (d != d) return 2;
    if (d - d != 0) return d > 0 ? -1 : 1;
    t = d >= 4503599627370496.0 || d <= -4503599627370496.0 ? d : (double)(int64_t)d; /* truncated */
    if ((c = rt_big_cmp(x, rt_big_from_double(t)))) return c;
    return t < d ? -1 : t > d;
}
/* a / b of nonzero exact integers as the nearest double: q = |a| * 2^s / |b| with >= 66 bits. */
static double big_ratio(const big_v *a, const big_v *b) {
    intptr_t s = 66 + big_bits(b->d, b->n) - big_bits(a->d, a->n), w, nq;
    rt_bignum *t;
    big_v x;
    obj q, r;
    if (s < 0) s = 0;
    w = s / 32;
    t = big_new(a->n + w + 1);
    mag_shl(t->d + w, a->d, a->n, (int)(s % 32));
    x.d = t->d, x.n = a->n + w + 1, x.neg = 0;
    while (x.n && !x.d[x.n - 1]) x.n--;
    big_divmod(&x, b, &q, &r);
    big_view(q, &x);
    nq = x.n;
    return big_dbl(x.d, nq, a->neg != b->neg, s, r != RT_FIX(0));
}
RT_FN obj rt_big_div(obj a, obj b) {
    big_v x, y;
    obj q, r;
    big_view(a, &x);
    big_view(b, &y);
    if (!y.n) rt_error("/", "division by zero", rt_cons(a, rt_cons(b, RT_NIL)));
    big_divmod(&x, &y, &q, &r);
    if (r == RT_FIX(0)) return q;
    return rt_make_flonum(big_ratio(&x, &y));
}
/* quotient remainder modulo floor-quotient; integral flonums give inexact results */
RT_FN obj rt_big_idiv(obj a, obj b, int op, const char *who) {
    big_v x, y;
    obj q = RT_FIX(0), r = RT_FIX(0), ab[2];
    int i, inexact = 0;
    ab[0] = a, ab[1] = b;
    for (i = 0; i < 2; i++)
        if (!rt_is_exact(ab[i])) {
            double d = rt_is_flo(ab[i]) ? RT_FLO(ab[i]) : 0.5;
            if (!(d - d == 0 && (d >= 4503599627370496.0 || d <= -4503599627370496.0 || (double)(int64_t)d == d)))
                rt_type_error(who, "integer", ab[i]);
            ab[i] = rt_big_from_double(d);
            inexact = 1;
        }
    big_view(ab[0], &x);
    big_view(ab[1], &y);
    if (!y.n) rt_error(who, "division by zero", rt_cons(a, rt_cons(b, RT_NIL)));
    big_divmod(&x, &y, op == 1 || op == 2 ? NULL : &q, op == 0 ? NULL : &r);
    if (op >= 2 && r != RT_FIX(0) && (rt_big_cmp(r, RT_FIX(0)) < 0) != y.neg) { /* floor: toward -inf */
        if (op == 2) r = rt_add(r, ab[1]);
        else q = rt_sub(q, RT_FIX(1));
    }
    r = op == 0 || op == 3 ? q : r;
    return inexact ? rt_make_flonum(rt_num_dbl(r)) : r;
}

/* expt with exact operands: square and multiply (the generic * promotes) */
RT_FN obj rt_big_expt(obj a, obj b) {
    obj r = RT_FIX(1);
    intptr_t e;
    big_v x;
    if (a == RT_FIX(0) || a == RT_FIX(1) || a == RT_FIX(-1)) {
        int neg = rt_big_cmp(b, RT_FIX(0)) < 0, odd = rt_is_fix(b) ? (RT_FIXVAL(b) & 1) : (int)(RT_PTR(b, rt_bignum)->d[0] & 1);
        if (a == RT_FIX(0) && neg) rt_error("expt", "division by zero", rt_cons(a, rt_cons(b, RT_NIL)));
        return a == RT_FIX(-1) && !odd ? RT_FIX(1) : b == RT_FIX(0) ? RT_FIX(1) : a;
    }
    big_view(a, &x);
    if (rt_is_big(b)) {
        if (RT_HFLAGS(b) & RT_F_NEG) return rt_make_flonum(0.0);
        rt_error("expt", "result too large", rt_cons(a, rt_cons(b, RT_NIL)));
    }
    e = RT_FIXVAL(b);
    if (e < 0) return rt_div(RT_FIX(1), rt_big_expt(a, RT_FIX(-e)));
    if ((double)e * (double)(big_bits(x.d, x.n) - 1) > 68719476736.0) /* 2^36 bits: 8 GB */
        rt_error("expt", "result too large", rt_cons(a, rt_cons(b, RT_NIL)));
    for (;;) {
        if (e & 1) r = rt_mul(r, a);
        if ((e >>= 1) == 0) return r;
        a = rt_mul(a, a);
    }
}

/* ---- text ---- */
RT_FN obj rt_big_to_string(obj x, int radix) {
    big_v v;
    intptr_t n, cap, pos, nb, i;
    char *buf, *p;
    big_view(x, &v);
    n = v.n;
    cap = 32 * n + 2;
    buf = (char *)rt_alloc_raw((size_t)cap);
    p = buf + cap;
    if (radix == 10) { /* nine digits per division: O(n^2) */
        rt_bignum *t = big_new(n);
        rt_memcpy(t->d, v.d, 4 * (size_t)n);
        while (n) {
            big_l r = mag_div1(t->d, t->d, n, 1000000000u);
            while (n && !t->d[n - 1]) n--;
            for (i = 0; i < 9 && (n || r); i++) { *--p = (char)('0' + r % 10); r /= 10; }
        }
    } else {
        int k = radix == 2 ? 1 : radix == 8 ? 3 : 4;
        nb = big_bits(v.d, n);
        for (pos = 0; pos < nb; pos += k) {
            uint64_t w = v.d[pos / 32] | (pos / 32 + 1 < n ? (uint64_t)v.d[pos / 32 + 1] << 32 : 0);
            *--p = "0123456789abcdef"[(w >> (pos % 32)) & (uint64_t)(radix - 1)];
        }
    }
    if (v.neg) *--p = '-';
    return rt_make_string_n(p, (size_t)(buf + cap - p));
}
RT_FN obj rt_big_parse(const char *s, const char *e, int radix, int neg) {
    intptr_t n = (e - s) * (radix == 2 ? 1 : radix == 8 ? 3 : 4) / 32 + 2, len = 0, i;
    rt_bignum *r = big_new(n);
    big_l m = 1, acc = 0;
    for (;; s++) {
        int c = s < e ? *s | 0x20 : 0, d = c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
        if (s == e || m > 0xFFFFFFFFu / (big_l)radix) { /* r = r * m + acc */
            uint64_t k = acc;
            for (i = 0; i < len; i++) { k += (uint64_t)r->d[i] * m; r->d[i] = (big_l)k; k >>= 32; }
            if (k) r->d[len++] = (big_l)k;
            m = 1, acc = 0;
            if (s == e) break;
        }
        m *= (big_l)radix;
        acc = acc * (big_l)radix + (big_l)d;
    }
    return big_fin(r, n, neg);
}

/* ---- roots and logarithms ---- */
RT_FN obj rt_exact_isqrt(obj k) { /* Newton from above: x0 = 2^ceil(bits/2) >= sqrt(k) */
    big_v v;
    obj x, y;
    if (!rt_is_exact(k) || rt_big_cmp(k, RT_FIX(0)) < 0) rt_type_error("exact-integer-sqrt", "exact nonnegative integer", k);
    big_view(k, &v);
    if (v.n == 0) return k;
    x = rt_big_expt(RT_FIX(2), RT_FIX((big_bits(v.d, v.n) + 1) / 2));
    for (;;) {
        y = rt_quotient(rt_add(x, rt_quotient(k, x)), RT_FIX(2));
        if (rt_big_cmp(y, x) >= 0) return x;
        x = y;
    }
}
RT_FN double rt_big_sqrt(obj x) { /* beyond 2^1000: the root of the integer root (within a unit of it) */
    big_v v;
    big_view(x, &v);
    if (v.neg || big_bits(v.d, v.n) < 1000) return rt_sys_sqrt(rt_big_to_double(x));
    return rt_big_to_double(rt_exact_isqrt(x));
}
RT_FN double rt_big_log(obj x) { /* log(m * 2^(bits-64)) with m the leading 64 bits */
    big_v v;
    intptr_t sh;
    big_view(x, &v);
    if (v.neg) return rt_sys_log(-1.0);
    sh = big_bits(v.d, v.n) - 64;
    return rt_sys_log(big_dbl(v.d, v.n, 0, sh, 0)) + (double)sh * 0.6931471805599453;
}
#endif
