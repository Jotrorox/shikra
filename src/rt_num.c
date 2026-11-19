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

/* ---- decimal conversion ----
 * TODO(M5): interim algorithm. m * 10^e is computed in double-double arithmetic
 * (~106-bit significand) and then rounded once, so parsing of <= 19 significant digits
 * is correctly rounded except for inputs within ~2^-100 of a halfway point or in the
 * subnormal range; further digits are truncated. Printing tries precisions 1..17 and
 * keeps the first whose digits parse back to the same double. M5 replaces both with
 * shortest round-trip printing and correctly rounded parsing (bignum fallback). */
typedef struct { double hi, lo; } num_dd;
static const double num_p10[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
static num_dd dd_make(double hi, double lo) { num_dd r; r.hi = hi + lo; r.lo = lo - (r.hi - hi); return r; }
static void dd_split(double a, double *h, double *l) { /* Dekker; scaled to avoid overflow */
    double s = (a > 1e300 || a < -1e300) ? 268435456.0 : 1.0, t;
    a /= s;
    t = 134217729.0 * a;
    *h = (t - (t - a)) * s;
    *l = (a - (t - (t - a))) * s;
}
static num_dd dd_mul(num_dd a, num_dd b) {
    double p = a.hi * b.hi, ah, al, bh, bl, e;
    if (p == NUM_INF || p == 0) return dd_make(p, 0);
    dd_split(a.hi, &ah, &al);
    dd_split(b.hi, &bh, &bl);
    e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return dd_make(p, e + a.hi * b.lo + a.lo * b.hi);
}
static num_dd dd_sub(num_dd a, num_dd b) {
    double s = a.hi - b.hi, bb = s - a.hi, e = (a.hi - (s - bb)) - (b.hi + bb);
    return dd_make(s, e + a.lo - b.lo);
}
static num_dd dd_div(num_dd a, num_dd b) {
    double q1 = a.hi / b.hi, q2, q3;
    num_dd r = dd_sub(a, dd_mul(b, dd_make(q1, 0)));
    q2 = r.hi / b.hi;
    r = dd_sub(r, dd_mul(b, dd_make(q2, 0)));
    q3 = r.hi / b.hi;
    r = dd_make(q1, q2);
    return dd_make(r.hi, r.lo + q3);
}
static num_dd dd_pow10(long k) { /* 0 <= k <= 300 */
    num_dd r = dd_make(num_p10[k % 22], 0);
    for (; k >= 22; k -= 22) r = dd_mul(r, dd_make(1e22, 0));
    return r;
}
static num_dd dd_u64(uint64_t m) { /* exact for m < 2^63 */
    double hi = (double)m;
    return dd_make(hi, (double)(int64_t)(m - (uint64_t)hi));
}
static num_dd num_scale(num_dd x, long e) { /* x * 10^e */
    double post = 1; /* keep intermediates normal: Dekker splitting fails on subnormals */
    if (x.hi == 0) return x;
    if (x.hi < 1e-290 || (e < -290 && x.hi < 1e20)) { x.hi *= 0x1p200; x.lo *= 0x1p200; post = 0x1p-200; }
    for (; e > 300; e -= 300) x = dd_mul(x, dd_pow10(300));
    for (; e < -300; e += 300) x = dd_div(x, dd_pow10(300));
    x = e >= 0 ? dd_mul(x, dd_pow10(e)) : dd_div(x, dd_pow10(-e));
    x.hi *= post;
    x.lo *= post;
    return x;
}
static uint64_t dd_round(num_dd x) { /* nearest integer of 0 <= x < 2^63 */
    uint64_t m = (uint64_t)x.hi;
    double g = (x.hi - (double)m) + x.lo + 0.5; /* |g| is small: hi is within an ulp */
    int64_t k = (int64_t)g;
    if ((double)k > g) k--;
    return m + (uint64_t)k;
}

RT_FN size_t rt_format_double(double d, char *out) {
    char dig[24], *o = out;
    uint64_t m = 0, pw;
    int p, e10, e2, i, n, tries;
    if (d != d) { rt_memcpy(out, "+nan.0", 6); return 6; }
    if (num_dbits(d) >> 63) { *o++ = '-'; d = -d; }
    if (d == NUM_INF) { if (o == out) *o++ = '+'; rt_memcpy(o, "inf.0", 5); return (size_t)(o - out) + 5; }
    if (d == 0) { rt_memcpy(o, "0.0", 3); return (size_t)(o - out) + 3; }
    e2 = d < 2.2250738585072014e-308 ? (int)((num_dbits(d * 0x1p64) >> 52) & 0x7FF) - 1023 - 64 /* subnormal */
                                     : (int)((num_dbits(d) >> 52) & 0x7FF) - 1023;
    e10 = e2 >= 0 ? (e2 * 78913) >> 18 : -((-e2 * 78913 + (1 << 18) - 1) >> 18); /* ~floor(e2 log10 2) */
    for (p = 1; p <= 17; p++) {
        for (pw = 1, i = 1; i < p; i++) pw *= 10;
        for (tries = 0; tries < 4; tries++) {
            m = dd_round(num_scale(dd_make(d, 0), (long)(p - 1 - e10)));
            if (m >= pw * 10) e10++;
            else if (m < pw) e10--;
            else break;
        }
        if (num_scale(dd_u64(m), (long)(e10 - p + 1)).hi == d) break;
    }
    if (p > 17) p = 17;
    while (p > 1 && m % 10 == 0) { m /= 10; p--; }
    for (i = p - 1; i >= 0; i--) { dig[i] = (char)('0' + m % 10); m /= 10; }
    if (e10 >= -7 && e10 < 21) { /* positional */
        if (e10 < 0) {
            *o++ = '0'; *o++ = '.';
            for (i = -1; i > e10; i--) *o++ = '0';
            for (i = 0; i < p; i++) *o++ = dig[i];
        } else {
            for (i = 0; i <= e10; i++) *o++ = i < p ? dig[i] : '0';
            *o++ = '.';
            if (p > e10 + 1) for (i = e10 + 1; i < p; i++) *o++ = dig[i];
            else *o++ = '0';
        }
    } else {
        *o++ = dig[0];
        if (p > 1) { *o++ = '.'; for (i = 1; i < p; i++) *o++ = dig[i]; }
        *o++ = 'e';
        if (e10 < 0) { *o++ = '-'; e10 = -e10; }
        n = e10 >= 100 ? 3 : e10 >= 10 ? 2 : 1;
        for (i = n - 1; i >= 0; i--) { o[i] = (char)('0' + e10 % 10); e10 /= 10; }
        o += n;
    }
    return (size_t)(o - out);
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

static int num_digit(int c, int radix) {
    int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : 99;
    return d < radix ? d : -1;
}
static int num_ci(const char *s, const char *lit) { /* s has at least strlen(lit) chars */
    for (; *lit; s++, lit++) if ((*s | 0x20) != *lit) return 0;
    return 1;
}

static int num_real(const char *s, const char *e, int radix, int exactness, obj *out) {
    int neg = 0, sign = 0, d, ndig = 0, inexact = 0, ovf = 0;
    long dexp = 0, ex = 0;
    uint64_t mant = 0, lim = radix == 10 ? 999999999999999999u : (UINT64_MAX - (uint64_t)(radix - 1)) / (uint64_t)radix;
    double dv = 0, v;
    if (s < e && (*s == '+' || *s == '-')) { neg = *s == '-'; sign = 1; s++; }
    if (sign && e - s == 5 && (num_ci(s, "inf.0") || num_ci(s, "nan.0"))) {
        if (exactness == 'e') return -2;
        v = (s[0] | 0x20) == 'i' ? NUM_INF : NUM_NAN;
        *out = rt_make_flonum(neg ? -v : v);
        return 0;
    }
    for (; s < e && (d = num_digit(*s, radix)) >= 0; s++, ndig++) {
        dv = dv * radix + d;
        if (mant <= lim) mant = mant * (uint64_t)radix + (uint64_t)d;
        else { ovf = 1; dexp++; }
    }
    if (radix == 10 && s < e && *s == '.') {
        inexact = 1;
        for (s++; s < e && (d = num_digit(*s, 10)) >= 0; s++, ndig++)
            if (mant <= lim) { mant = mant * 10 + (uint64_t)d; dexp--; }
    }
    if (ndig == 0) return -1;
    if (radix == 10 && s < e && (*s | 0x20) == 'e') {
        int eneg = 0, nd = 0;
        inexact = 1;
        if (++s < e && (*s == '+' || *s == '-')) eneg = *s++ == '-';
        for (; s < e && (d = num_digit(*s, 10)) >= 0; s++, nd++) if (ex < 100000) ex = ex * 10 + d;
        if (!nd) return -1;
        if (eneg) ex = -ex;
    }
    if (s < e && *s == '/' && !inexact) { /* no rationals: exact only when it divides */
        uint64_t den = 0;
        double dd = 0;
        int dovf = 0, nd = 0;
        for (s++; s < e && (d = num_digit(*s, radix)) >= 0; s++, nd++) {
            dd = dd * radix + d;
            if (den <= (UINT64_MAX - (uint64_t)d) / (uint64_t)radix) den = den * (uint64_t)radix + (uint64_t)d;
            else dovf = 1;
        }
        if (!nd || s != e) return -1;
        if (dd == 0) return -2;
        if (exactness != 'i') { /* both parts must be exact integers Shikra can represent */
            if (ovf || dovf || mant > (uint64_t)RT_FIX_MAX + (uint64_t)neg || den > (uint64_t)RT_FIX_MAX) return -2;
            if (mant % den == 0) { mant /= den; goto exact; }
        }
        if (exactness == 'e') return -2;
        v = dv / dd;
        *out = rt_make_flonum(neg ? -v : v);
        return 0;
    }
    if (s != e) return -1;
    if (!inexact && exactness != 'i') goto exact;
    v = radix == 10 ? num_scale(dd_u64(mant), dexp + ex).hi : dv;
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
