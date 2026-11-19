/* rt_num.c -- numbers: 63-bit fixnums and boxed IEEE doubles (spec §3.2, §6.2).
 * Fixnum overflow raises an error and never wraps. No rationals: (/ 1 3) is a flonum,
 * (/ 6 3) is the exact 2. No libm and no libc: rounding is done by hand, and float
 * printing/parsing uses the interim algorithm below. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

RT_FN obj rt_make_flonum(double d) {
    rt_flonum *f = (rt_flonum *)rt_alloc(RT_HDR(RT_T_FLONUM, 0, 8), sizeof(rt_flonum));
    f->d = d;
    return RT_TAGPTR(f);
}

static double num_bits(uint64_t u) { union { uint64_t u; double d; } x; x.u = u; return x.d; }
static uint64_t num_dbits(double d) { union { uint64_t u; double d; } x; x.d = d; return x.u; }
#define NUM_INF num_bits(0x7FF0000000000000u)
#define NUM_NAN num_bits(0x7FF8000000000000u)
#define NUM_2P52 4503599627370496.0
#define NUM_2P62 4611686018427387904.0

static double num_d(obj x, const char *who) {
    if (rt_is_fix(x)) return (double)RT_FIXVAL(x);
    RT_CHECK(rt_is_flo(x), who, "number", x);
    return RT_FLO(x);
}
static RT_NORETURN void num_overflow(const char *who, obj a, obj b) {
    rt_error(who, "fixnum overflow", rt_cons(a, rt_cons(b, RT_NIL)));
}
static obj num_fix(intptr_t v, const char *who, obj a, obj b) {
    if (v < RT_FIX_MIN || v > RT_FIX_MAX) num_overflow(who, a, b);
    return RT_FIX(v);
}

/* ---- arithmetic (slow paths of the inline rt_add & co. in rt.h) ---- */
RT_FN obj rt_add2(obj a, obj b) {
    if (rt_is_fix(a) && rt_is_fix(b)) num_overflow("+", a, b);
    return rt_make_flonum(num_d(a, "+") + num_d(b, "+"));
}
RT_FN obj rt_sub2(obj a, obj b) {
    if (rt_is_fix(a) && rt_is_fix(b)) num_overflow("-", a, b);
    return rt_make_flonum(num_d(a, "-") - num_d(b, "-"));
}
RT_FN obj rt_mul2(obj a, obj b) {
    if (rt_is_fix(a) && rt_is_fix(b)) num_overflow("*", a, b);
    return rt_make_flonum(num_d(a, "*") * num_d(b, "*"));
}
RT_FN obj rt_div(obj a, obj b) {
    if (rt_is_fix(a) && rt_is_fix(b)) {
        intptr_t x = RT_FIXVAL(a), y = RT_FIXVAL(b);
        if (y == 0) rt_error("/", "division by zero", rt_cons(a, rt_cons(b, RT_NIL)));
        if (x % y == 0) return num_fix(x / y, "/", a, b);
    }
    return rt_make_flonum(num_d(a, "/") / num_d(b, "/"));
}

/* Exact comparison of a fixnum with a non-NaN double. */
static int num_cmp_id(intptr_t i, double d) {
    intptr_t t;
    double f;
    if (d >= 9223372036854775808.0) return -1;
    if (d < -9223372036854775808.0) return 1;
    t = (intptr_t)d;
    if (i != t) return i < t ? -1 : 1;
    f = d - (double)t;
    return f > 0 ? -1 : f < 0;
}
RT_FN int rt_numcmp(obj a, obj b, const char *who) {
    double x, y;
    if (rt_is_fix(a) && rt_is_fix(b)) return (intptr_t)a < (intptr_t)b ? -1 : (intptr_t)a > (intptr_t)b;
    x = num_d(a, who);
    y = num_d(b, who);
    if (x != x || y != y) return 2;
    if (rt_is_fix(a)) return num_cmp_id(RT_FIXVAL(a), y);
    if (rt_is_fix(b)) return -num_cmp_id(RT_FIXVAL(b), x);
    return x < y ? -1 : x > y;
}
RT_FN obj rt_num_eq(obj a, obj b) { return RT_BOOL(rt_numcmp(a, b, "=") == 0); }
RT_FN obj rt_lt2(obj a, obj b) { return RT_BOOL(rt_numcmp(a, b, "<") == -1); }
RT_FN obj rt_gt2(obj a, obj b) { return RT_BOOL(rt_numcmp(a, b, ">") == 1); }
RT_FN obj rt_num_le(obj a, obj b) { int c = rt_numcmp(a, b, "<="); return RT_BOOL(c == -1 || c == 0); }
RT_FN obj rt_num_ge(obj a, obj b) { int c = rt_numcmp(a, b, ">="); return RT_BOOL(c == 1 || c == 0); }

/* ---- rounding without libm ---- */
static double num_trunc(double x) {
    double t;
    if (!(x > -NUM_2P52 && x < NUM_2P52)) return x; /* NaN, infinite or already integral */
    t = (double)(int64_t)x;
    return t == 0 ? x * 0.0 : t; /* keep the sign of zero */
}
static double num_floor(double x) { double t = num_trunc(x); return t > x ? t - 1 : t; }
static double num_round(double x) { /* round half to even */
    double f = num_floor(x), r = x - f;
    if (r > 0.5 || (r == 0.5 && num_trunc(f / 2) != f / 2)) f += 1;
    return f == 0 ? x * 0.0 : f;
}
static obj num_unary(obj x, int op, const char *who) {
    double d, r;
    if (rt_is_fix(x)) return x;
    d = num_d(x, who);
    switch (op) {
    case 0: r = num_floor(d); break;
    case 1: r = -num_floor(-d); break;
    case 2: r = num_round(d); break;
    default: r = num_trunc(d); break;
    }
    return rt_make_flonum(r);
}
RT_FN obj rt_floor(obj x) { return num_unary(x, 0, "floor"); }
RT_FN obj rt_ceiling(obj x) { return num_unary(x, 1, "ceiling"); }
RT_FN obj rt_round(obj x) { return num_unary(x, 2, "round"); }
RT_FN obj rt_truncate(obj x) { return num_unary(x, 3, "truncate"); }

/* ---- integer division: fixnums, or integral flonums (result inexact) ---- */
static intptr_t num_int(obj x, const char *who, int *inexact) {
    double d;
    if (rt_is_fix(x)) return RT_FIXVAL(x);
    d = num_d(x, who);
    if (num_trunc(d) != d || !(d > -NUM_2P62 && d < NUM_2P62)) rt_type_error(who, "integer", x);
    *inexact = 1;
    return (intptr_t)d;
}
static obj num_idiv(obj a, obj b, int op, const char *who) {
    int inexact = 0;
    intptr_t x = num_int(a, who, &inexact), y = num_int(b, who, &inexact), r;
    if (y == 0) rt_error(who, "division by zero", rt_cons(a, rt_cons(b, RT_NIL)));
    switch (op) {
    case 0: r = x / y; break;                                         /* truncate-quotient */
    case 1: r = x % y; break;                                         /* truncate-remainder */
    case 2: r = x % y; if (r != 0 && (r < 0) != (y < 0)) r += y; break; /* floor-remainder */
    default: r = x / y; if (x % y != 0 && (x < 0) != (y < 0)) r--; break; /* floor-quotient */
    }
    return inexact ? rt_make_flonum((double)r) : num_fix(r, who, a, b);
}
RT_FN obj rt_quotient(obj a, obj b) { return num_idiv(a, b, 0, "quotient"); }
RT_FN obj rt_remainder(obj a, obj b) { return num_idiv(a, b, 1, "remainder"); }
RT_FN obj rt_modulo(obj a, obj b) { return num_idiv(a, b, 2, "modulo"); }
RT_FN obj rt_floor_quotient(obj a, obj b) { return num_idiv(a, b, 3, "floor-quotient"); }

/* Negation and abs: the most negative fixnum has no fixnum opposite. */
static obj num_neg(obj x, const char *who) {
    if (rt_is_fix(x)) {
        if (RT_FIXVAL(x) == RT_FIX_MIN) rt_error(who, "fixnum overflow", rt_cons(x, RT_NIL));
        return RT_FIX(-RT_FIXVAL(x));
    }
    return rt_make_flonum(-num_d(x, who));
}
RT_FN obj rt_neg(obj x) { return num_neg(x, "-"); }
RT_FN obj rt_abs(obj x) {
    if (rt_is_fix(x) ? RT_FIXVAL(x) >= 0 : (num_dbits(num_d(x, "abs")) >> 63) == 0) return x;
    return num_neg(x, "abs");
}

/* expt: exact base and exponent stay exact (overflow raises); a negative exact exponent
 * gives 1/b^-e, exact when that divides, like /; anything inexact goes to pow. */
RT_FN obj rt_expt(obj a, obj b) {
    double x = num_d(a, "expt"), y = num_d(b, "expt");
    if (rt_is_fix(a) && rt_is_fix(b)) {
        intptr_t base = RT_FIXVAL(a), e = RT_FIXVAL(b), r = 1, ovf = 0;
        if (e < 0) {
            if (base == 0) rt_error("expt", "division by zero", rt_cons(a, rt_cons(b, RT_NIL)));
            if (base == 1 || base == -1) return RT_FIX(base == 1 || e % 2 == 0 ? 1 : -1);
            return rt_make_flonum(rt_sys_pow(x, y));
        }
        for (;;) {
            if ((e & 1) && rt_mul_ovf(r, base, &r)) ovf = 1;
            if ((e >>= 1) == 0) break;
            if (rt_mul_ovf(base, base, &base)) ovf = 1;
        }
        if (ovf || r < RT_FIX_MIN || r > RT_FIX_MAX) rt_error("expt", "fixnum overflow", rt_cons(a, rt_cons(b, RT_NIL)));
        return RT_FIX(r);
    }
    return rt_make_flonum(rt_sys_pow(x, y));
}

RT_FN obj rt_exact(obj x) {
    double d;
    if (rt_is_fix(x)) return x;
    d = num_d(x, "exact");
    if (num_trunc(d) != d || !(d >= -NUM_2P62 && d < NUM_2P62)) rt_error("exact", "no exact representation", rt_cons(x, RT_NIL));
    return RT_FIX((intptr_t)d);
}
RT_FN obj rt_inexact(obj x) { return rt_is_flo(x) ? x : rt_make_flonum(num_d(x, "inexact")); }
RT_FN obj rt_integer_p(obj x) { return RT_BOOL(rt_is_fix(x) || (rt_is_flo(x) && num_trunc(RT_FLO(x)) == RT_FLO(x) && RT_FLO(x) - RT_FLO(x) == 0)); }
RT_FN obj rt_nan_p(obj x) { double d = num_d(x, "nan?"); return RT_BOOL(d != d); }
RT_FN obj rt_infinite_p(obj x) { double d = num_d(x, "infinite?"); return RT_BOOL(d == NUM_INF || d == -NUM_INF); }
RT_FN obj rt_finite_p(obj x) { double d = num_d(x, "finite?"); return RT_BOOL(d - d == 0); }

/* ---- (scheme inexact): through the platform layer (rt_sys.c) ---- */
#define NUM_MATH1(name, who, f) RT_FN obj name(obj x) { return rt_make_flonum(f(num_d(x, who))); }
NUM_MATH1(rt_exp, "exp", rt_sys_exp)
NUM_MATH1(rt_log, "log", rt_sys_log)
NUM_MATH1(rt_sin, "sin", rt_sys_sin)
NUM_MATH1(rt_cos, "cos", rt_sys_cos)
NUM_MATH1(rt_tan, "tan", rt_sys_tan)
NUM_MATH1(rt_asin, "asin", rt_sys_asin)
NUM_MATH1(rt_acos, "acos", rt_sys_acos)
NUM_MATH1(rt_sqrt, "sqrt", rt_sys_sqrt)
RT_FN obj rt_atan(obj x) { return rt_make_flonum(rt_sys_atan2(num_d(x, "atan"), 1.0)); }
RT_FN obj rt_atan2(obj y, obj x) { return rt_make_flonum(rt_sys_atan2(num_d(y, "atan"), num_d(x, "atan"))); }

/* ---- decimal conversion (spec §9.1 "Float printing and parsing"; DESIGN.md "M5 as built") ----
 * Both directions are exact, on small fixed-size big integers (32-bit limbs, up to 4160 bits:
 * enough for every double and for the 780 significant digits that parsing keeps).
 * Printing: Burger & Dybvig's free-format algorithm -- the shortest digit string that reads
 * back to the same double (a tie in the last digit rounds up), with the decimal exponent
 * estimated from the bit length. Parsing: the number is an exact ratio num/den; its 54
 * leading quotient bits plus a sticky bit round to nearest even, into the subnormal range
 * too. Digits beyond the 780th only set the sticky bit (a point halfway between two doubles
 * has at most 767 significant digits). */
#define NUM_BN 130
#define NUM_MAXDIG 780
typedef struct { int n; uint32_t d[NUM_BN]; } num_big; /* little-endian limbs, n used, normalised */
static const uint32_t num_p10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

static RT_NOINLINE void bn_set(num_big *a, uint64_t v) {
    a->d[0] = (uint32_t)v;
    a->d[1] = (uint32_t)(v >> 32);
    a->n = v >> 32 ? 2 : v ? 1 : 0;
}
static RT_NOINLINE void bn_muladd(num_big *a, uint32_t m, uint32_t add) { /* a = a * m + add */
    uint64_t c = add;
    int i;
    for (i = 0; i < a->n; i++) { c += (uint64_t)a->d[i] * m; a->d[i] = (uint32_t)c; c >>= 32; }
    if (c) a->d[a->n++] = (uint32_t)c;
}
static RT_NOINLINE void bn_pow10(num_big *a, long k) { /* a *= 10^k, k >= 0 */
    for (; k >= 9; k -= 9) bn_muladd(a, 1000000000u, 0);
    bn_muladd(a, num_p10[k], 0);
}
static RT_NOINLINE int bn_bits(const num_big *a) {
    int b = 32 * a->n;
    uint32_t t;
    if (!a->n) return 0;
    for (t = a->d[a->n - 1]; !(t & 0x80000000u); t <<= 1) b--;
    return b;
}
static RT_NOINLINE void bn_shl(num_big *a, int k) {
    int w = k >> 5, b = k & 31, i;
    uint32_t hi = b && a->n ? a->d[a->n - 1] >> (32 - b) : 0;
    if (!a->n) return;
    for (i = a->n - 1; i >= 0; i--) a->d[i + w] = (a->d[i] << b) | (b && i ? a->d[i - 1] >> (32 - b) : 0);
    for (i = 0; i < w; i++) a->d[i] = 0;
    a->n += w;
    if (hi) a->d[a->n++] = hi;
}
static RT_NOINLINE void bn_shr1(num_big *a) {
    int i;
    for (i = 0; i < a->n; i++) a->d[i] = (a->d[i] >> 1) | (i + 1 < a->n ? a->d[i + 1] << 31 : 0);
    if (a->n && !a->d[a->n - 1]) a->n--;
}
static RT_NOINLINE int bn_cmp(const num_big *a, const num_big *b) {
    int i;
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static RT_NOINLINE void bn_sub(num_big *a, const num_big *b) { /* a -= b, a >= b */
    uint64_t br = 0, t;
    int i;
    for (i = 0; i < a->n; i++) {
        t = (uint64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - br;
        a->d[i] = (uint32_t)t;
        br = t >> 63;
    }
    while (a->n && !a->d[a->n - 1]) a->n--;
}
static RT_NOINLINE void bn_add(num_big *r, const num_big *a, const num_big *b) { /* r = a + b (r may be a) */
    int i, n = a->n > b->n ? a->n : b->n;
    uint64_t c = 0;
    for (i = 0; i < n; i++) { c += (uint64_t)(i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0); r->d[i] = (uint32_t)c; c >>= 32; }
    r->n = n;
    if (c) r->d[r->n++] = (uint32_t)c;
}

/* Shortest round-trip digits: d = 0.D1D2...Dn * 10^k. Positional notation for
 * 1e-7 <= |d| < 1e21, otherwise D1.D2...Dn e<sign><exponent>, always with a fraction digit. */
RT_FN size_t rt_format_double(double d, char *out) {
    num_big r, s, mp, mm, t;
    char dig[20], *o = out;
    uint64_t bits = num_dbits(d), f = bits & 0xFFFFFFFFFFFFFu;
    int be = (int)(bits >> 52) & 0x7FF, e, w, k, n = 0, i, x, even, lo, hi;
    int64_t est;
    if (d != d) { rt_memcpy(out, "+nan.0", 6); return 6; }
    if (bits >> 63) *o++ = '-';
    if (be == 0x7FF) { if (o == out) *o++ = '+'; rt_memcpy(o, "inf.0", 5); return (size_t)(o - out) + 5; }
    if (!be && !f) { rt_memcpy(o, "0.0", 3); return (size_t)(o - out) + 3; }
    w = f == 0 && be > 1; /* a power of two: the gap below is half the gap above */
    if (be) f |= (uint64_t)1 << 52;
    else be = 1;
    e = be - 1075; /* d = f * 2^e; r/s = d, mp/s and mm/s = the half gaps above and below */
    even = !(f & 1);
    bn_set(&r, f << (1 + w));
    bn_set(&s, (uint64_t)2 << w);
    bn_set(&mp, (uint64_t)1 << w);
    bn_set(&mm, 1);
    if (e >= 0) { bn_shl(&r, e); bn_shl(&mp, e); bn_shl(&mm, e); }
    else bn_shl(&s, -e);
    for (x = 0; f >> x; x++) {}
    est = (int64_t)(x + e - 1) * 1292913986; /* k ~ ceil(log10 d), never too large */
    k = est >= 0 ? (int)((est + 0xFFFFFFFF) >> 32) : -(int)((-est) >> 32);
    if (k >= 0) bn_pow10(&s, k);
    else { bn_pow10(&r, -k); bn_pow10(&mp, -k); bn_pow10(&mm, -k); }
    bn_add(&t, &r, &mp);
    if (bn_cmp(&t, &s) >= !even) k++;
    else { bn_muladd(&r, 10, 0); bn_muladd(&mp, 10, 0); bn_muladd(&mm, 10, 0); }
    for (;;) {
        for (x = 0; bn_cmp(&r, &s) >= 0; x++) bn_sub(&r, &s);
        bn_add(&t, &r, &mp);
        lo = bn_cmp(&r, &mm) < even;
        hi = bn_cmp(&t, &s) >= !even;
        if (lo || hi) {
            if (hi && (!lo || (bn_shl(&r, 1), bn_cmp(&r, &s) >= 0))) x++;
            dig[n++] = (char)('0' + x);
            break;
        }
        dig[n++] = (char)('0' + x);
        bn_muladd(&r, 10, 0);
        bn_muladd(&mp, 10, 0);
        bn_muladd(&mm, 10, 0);
    }
    if (k >= -6 && k <= 21) {
        if (k <= 0) {
            *o++ = '0'; *o++ = '.';
            for (i = k; i < 0; i++) *o++ = '0';
            for (i = 0; i < n; i++) *o++ = dig[i];
        } else {
            for (i = 0; i < k || i < n; i++) {
                if (i == k) *o++ = '.';
                *o++ = i < n ? dig[i] : '0';
            }
            if (n <= k) { *o++ = '.'; *o++ = '0'; }
        }
        return (size_t)(o - out);
    }
    *o++ = dig[0];
    *o++ = '.';
    if (n == 1) *o++ = '0';
    for (i = 1; i < n; i++) *o++ = dig[i];
    *o++ = 'e';
    *o++ = k - 1 < 0 ? '-' : '+';
    x = k - 1 < 0 ? 1 - k : k - 1;
    n = x >= 100 ? 3 : x >= 10 ? 2 : 1;
    for (i = n - 1; i >= 0; i--) { o[i] = (char)('0' + x % 10); x /= 10; }
    return (size_t)(o + n - out);
}

RT_FN obj rt_number_to_string(obj x, obj radix) {
    char buf[72];
    size_t n;
    intptr_t r = rt_is_fix(radix) ? RT_FIXVAL(radix) : 0;
    if (r != 2 && r != 8 && r != 10 && r != 16) rt_type_error("number->string", "radix 2, 8, 10 or 16", radix);
    if (rt_is_fix(x)) {
        intptr_t v = RT_FIXVAL(x);
        uintptr_t u = v < 0 ? (uintptr_t)0 - (uintptr_t)v : (uintptr_t)v;
        char *p = buf + sizeof buf;
        do { *--p = "0123456789abcdef"[u % (uintptr_t)r]; u /= (uintptr_t)r; } while (u);
        if (v < 0) *--p = '-';
        return rt_make_string_n(p, (size_t)(buf + sizeof buf - p));
    }
    RT_CHECK(rt_is_flo(x), "number->string", "number", x);
    if (r != 10) rt_error("number->string", "inexact numbers are printed in radix 10 only", rt_cons(x, RT_NIL));
    n = rt_format_double(RT_FLO(x), buf);
    return rt_make_string_n(buf, n);
}

/* The double nearest to num/den (ties to even); num, den > 0; both are clobbered. */
static RT_NOINLINE double num_ratio(num_big *num, num_big *den) {
    int b = bn_bits(num) - bn_bits(den), s, i, sticky;
    uint64_t q = 0, m;
    if (b > 1026) return NUM_INF;
    if (b < -1080) return 0;
    s = 54 - b; /* num/den is in (2^(b-1), 2^(b+1)), so q = num * 2^s / den is in (2^53, 2^55) */
    if (s > 1075) s = 1075; /* subnormal: the last mantissa bit is worth 2^-1074 */
    if (s >= 0) bn_shl(num, s);
    else bn_shl(den, -s);
    bn_shl(den, 54);
    for (i = 0; i <= 54; i++) {
        q <<= 1;
        if (bn_cmp(num, den) >= 0) { bn_sub(num, den); q |= 1; }
        bn_shr1(den);
    }
    sticky = num->n != 0;
    if (q >> 54) { sticky |= (int)(q & 1); q >>= 1; s--; }
    m = q >> 1; /* the mantissa, last bit worth 2^(1-s); q & 1 is the rounding bit */
    if ((q & 1) && (sticky || (m & 1))) m++;
    m += (uint64_t)(1 - s + 1074) << 52; /* a carry out of the mantissa bumps the exponent */
    return m >= 0x7FF0000000000000u ? NUM_INF : num_bits(m);
}
/* dig[0..nd) / dd[0..ndd) (ndd = 0: 1) in radix, times 10^e10 (radix 10 only), plus a sticky
 * digit beyond the kept ones. */
static double num_convert(const char *dig, int nd, const char *dd, int ndd, int radix, long e10, int sticky) {
    num_big a, b;
    long m = nd + e10 - ndd; /* the value lies in (10^(m-2), 10^(m+1)) */
    int i;
    if (!nd) return 0;
    if (m > 310) return NUM_INF;
    if (m < -325) return 0;
    bn_set(&a, 0);
    for (i = 0; i < nd; i++) bn_muladd(&a, (uint32_t)radix, (uint32_t)dig[i]);
    if (sticky) { bn_muladd(&a, 10, 1); e10--; }
    bn_set(&b, 1);
    if (ndd) for (b.n = 0, i = 0; i < ndd; i++) bn_muladd(&b, (uint32_t)radix, (uint32_t)dd[i]);
    if (e10 > 0) bn_pow10(&a, e10);
    else bn_pow10(&b, -e10);
    return num_ratio(&a, &b);
}

static int num_digit(int c, int radix) {
    int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : 99;
    return d < radix ? d : -1;
}
static int num_ci(const char *s, const char *lit) { /* s has at least strlen(lit) chars */
    for (; *lit; s++, lit++) if ((*s | 0x20) != *lit) return 0;
    return 1;
}
/* Digits in radix: the value in *mant (*ovf when it exceeds 64 bits) and the significant digits
 * (at most NUM_MAXDIG, leading zeros dropped) in dig; *dexp counts digits dropped beyond the
 * cap (radix 10: a power of ten each; other radices: the number is too large), *sticky whether
 * any of them was nonzero. With frac, digits after a decimal point: each kept one lowers *dexp. */
static const char *num_digits(const char *s, const char *e, int radix, int frac, char *dig, int *nd,
                              long *dexp, int *sticky, uint64_t *mant, int *ovf, int *ndig) {
    int d;
    for (; s < e && (d = num_digit(*s, radix)) >= 0; s++, ++*ndig) {
        if (*mant > (UINT64_MAX - (uint64_t)d) / (uint64_t)radix) *ovf = 1;
        else *mant = *mant * (uint64_t)radix + (uint64_t)d;
        if (*nd < NUM_MAXDIG) {
            if (*nd || d) dig[(*nd)++] = (char)d;
            if (frac) --*dexp;
        } else {
            *sticky |= d;
            if (!frac) ++*dexp;
        }
    }
    return s;
}

static int num_real(const char *s, const char *e, int radix, int exactness, obj *out) {
    char dig[NUM_MAXDIG], den[NUM_MAXDIG];
    int neg = 0, sign = 0, nd = 0, ndig = 0, inexact = 0, ovf = 0, sticky = 0;
    long dexp = 0, ex = 0;
    uint64_t mant = 0;
    double v;
    if (s < e && (*s == '+' || *s == '-')) { neg = *s == '-'; sign = 1; s++; }
    if (sign && e - s == 5 && (num_ci(s, "inf.0") || num_ci(s, "nan.0"))) {
        if (exactness == 'e') return -2;
        v = (s[0] | 0x20) == 'i' ? NUM_INF : NUM_NAN;
        *out = rt_make_flonum(neg ? -v : v);
        return 0;
    }
    s = num_digits(s, e, radix, 0, dig, &nd, &dexp, &sticky, &mant, &ovf, &ndig);
    if (radix == 10 && s < e && *s == '.') {
        inexact = 1;
        s = num_digits(s + 1, e, 10, 1, dig, &nd, &dexp, &sticky, &mant, &ovf, &ndig);
    }
    if (ndig == 0) return -1;
    if (radix == 10 && s < e && ((*s | 0x20) == 'e' || (*s | 0x20) == 's' || (*s | 0x20) == 'f' ||
                                 (*s | 0x20) == 'd' || (*s | 0x20) == 'l')) { /* exponent markers */
        int eneg = 0, d, n = 0;
        inexact = 1;
        if (++s < e && (*s == '+' || *s == '-')) eneg = *s++ == '-';
        for (; s < e && (d = num_digit(*s, 10)) >= 0; s++, n++) if (ex < 100000) ex = ex * 10 + d;
        if (!n) return -1;
        if (eneg) ex = -ex;
    }
    if (radix != 10 && dexp > 0) ovf = 2; /* more digits than any representable number has */
    if (s < e && *s == '/' && !inexact) { /* no rationals: exact only when it divides */
        uint64_t dm = 0;
        int dovf = 0, nden = 0, ndd = 0, dsticky = 0;
        long ddexp = 0;
        s = num_digits(s + 1, e, radix, 0, den, &nden, &ddexp, &dsticky, &dm, &dovf, &ndd);
        if (!ndd || s != e) return -1;
        if (!nden) return -2;
        if (exactness != 'i') { /* both parts must be exact integers Shikra can represent */
            if (ovf || dovf || mant > (uint64_t)RT_FIX_MAX + (uint64_t)neg || dm > (uint64_t)RT_FIX_MAX) return -2;
            if (mant % dm == 0) { mant /= dm; goto exact; }
        }
        if (exactness == 'e' || ovf == 2 || dsticky || (radix != 10 && ddexp)) return -2;
        v = num_convert(dig, nd, den, nden, radix, dexp - ddexp, sticky);
        *out = rt_make_flonum(neg ? -v : v);
        return 0;
    }
    if (s != e) return -1;
    if (!inexact && exactness != 'i') goto exact;
    v = ovf == 2 ? NUM_INF : num_convert(dig, nd, 0, 0, radix, dexp + ex, sticky);
    if (exactness == 'e') {
        if (num_trunc(v) != v || !(v < NUM_2P62)) return -2;
        mant = (uint64_t)v;
        ovf = 0;
        goto exact;
    }
    *out = rt_make_flonum(neg ? -v : v);
    return 0;
exact:
    if (ovf || mant > (uint64_t)RT_FIX_MAX + (uint64_t)neg) return -2;
    *out = RT_FIX(neg ? (intptr_t)(0 - mant) : (intptr_t)mant);
    return 0;
}

RT_FN int rt_parse_number(const char *s, size_t n, int radix, obj *out) {
    const char *e = s + n;
    int exactness = 0, seen_radix = 0;
    while (e - s >= 2 && s[0] == '#') {
        int c = s[1] | 0x20;
        if (c == 'e' || c == 'i') { if (exactness) return -1; exactness = c; }
        else if (seen_radix) return -1;
        else if (c == 'x') radix = 16, seen_radix = 1;
        else if (c == 'b') radix = 2, seen_radix = 1;
        else if (c == 'o') radix = 8, seen_radix = 1;
        else if (c == 'd') radix = 10, seen_radix = 1;
        else return -1;
        s += 2;
    }
    return num_real(s, e, radix, exactness, out);
}

RT_FN obj rt_string_to_number(obj s, obj radix) {
    obj r;
    intptr_t rd = rt_is_fix(radix) ? RT_FIXVAL(radix) : 0;
    RT_CHECK(rt_is_string(s), "string->number", "string", s);
    if (rd != 2 && rd != 8 && rd != 10 && rd != 16) rt_type_error("string->number", "radix 2, 8, 10 or 16", radix);
    return rt_parse_number(RT_STR(s)->buf, (size_t)RT_STR(s)->bytes, (int)rd, &r) == 0 ? r : RT_FALSE;
}
