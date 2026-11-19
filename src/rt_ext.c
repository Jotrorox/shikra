/* rt_ext.c -- runtime of the (shikra ...) extension libraries (spec §10; DESIGN.md "Extensions as
 * built"): fixnum operations (SRFI 143), the bit-level and rounding operations of SRFI 144 flonums,
 * exact fmod / IEEE remainder, fused multiply-add, f64vectors (SRFI 4), failed assumptions
 * (SRFI 145) and the argument and result conversions of define-c. Everything else of the five
 * libraries is Scheme (lib/shikra/). No libc: the math functions come from the platform layer.
 *
 * Program mode: pasted right after rt.h, before the generated code, and only into programs that use
 * an extension (the emitter defines RT_EXT), so the generated code calls the inline helpers below
 * directly; everything is static, and what a program does not use disappears. The rt_fx_* / rt_fl_*
 * functions named in prims.def are the checked generic forms (the interpreter, unproven arguments);
 * the compiler emits the helpers on tagged words and doubles when the analysis proved the types. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif
#ifdef RT_HAVE_EXT

/* ---- fixnums: tagged words (n << 1) in intptr_t ---- */
RT_INLINE void rt_fxchk(obj x, const char *who) { RT_SAFE(if (RT_UNLIKELY(!rt_is_fix(x))) rt_type_error(who, "fixnum", x);) (void)x; (void)who; }
static RT_UNUSED RT_NORETURN void ext_ovf1(const char *who, intptr_t a) { rt_error(who, "fixnum overflow", rt_cons(RT_O(a), RT_NIL)); }
#ifdef RT_UNSAFE
#define EXT_OVF(c, call) ((void)(c))
#else
#define EXT_OVF(c, call) do { if (RT_UNLIKELY(c)) call; } while (0)
#endif
RT_INLINE RT_ALWAYS intptr_t rt_fxaddw(intptr_t a, intptr_t b, const char *w) { intptr_t r = 0; EXT_OVF(rt_add_ovf(a, b, &r), rt_fx_overflow(w, RT_O(a), RT_O(b))); (void)w; return r; }
RT_INLINE RT_ALWAYS intptr_t rt_fxsubw(intptr_t a, intptr_t b, const char *w) { intptr_t r = 0; EXT_OVF(rt_sub_ovf(a, b, &r), rt_fx_overflow(w, RT_O(a), RT_O(b))); (void)w; return r; }
RT_INLINE RT_ALWAYS intptr_t rt_fxmulw(intptr_t a, intptr_t b, const char *w) { intptr_t r = 0; EXT_OVF(rt_mul_ovf(RT_FIXVAL(a), b, &r), rt_fx_overflow(w, RT_O(a), RT_O(b))); (void)w; return r; }
#define EXT_FXMIN ((intptr_t)RT_FIX(RT_FIX_MIN))
RT_INLINE intptr_t rt_fxnegw(intptr_t a, const char *w) { EXT_OVF(a == EXT_FXMIN, ext_ovf1(w, a)); (void)w; return (intptr_t)(0 - (uintptr_t)a); }
RT_INLINE intptr_t rt_fxabsw(intptr_t a, const char *w) { return a < 0 ? rt_fxnegw(a, w) : a; }
RT_INLINE intptr_t rt_fxquow(intptr_t a, intptr_t b, const char *w) {
    (void)w;
    RT_SAFE(if (RT_UNLIKELY(b == 0)) rt_error(w, "division by zero", rt_cons(RT_O(a), rt_cons(RT_O(b), RT_NIL)));)
    EXT_OVF(a == EXT_FXMIN && b == (intptr_t)RT_FIX(-1), rt_fx_overflow(w, RT_O(a), RT_O(b)));
    return (intptr_t)RT_FIX(RT_FIXVAL(a) / RT_FIXVAL(b));
}
RT_INLINE intptr_t rt_fxremw(intptr_t a, intptr_t b, const char *w) {
    RT_SAFE(if (RT_UNLIKELY(b == 0)) rt_error(w, "division by zero", rt_cons(RT_O(a), rt_cons(RT_O(b), RT_NIL)));)
    (void)w;
    return (intptr_t)RT_FIX(RT_FIXVAL(a) % RT_FIXVAL(b));
}
/* shift counts: 0 <= k < fx-width (63) */
RT_INLINE intptr_t rt_fxshk(intptr_t n, intptr_t lim, const char *w) {
    RT_SAFE(if (RT_UNLIKELY((uintptr_t)(RT_FIXVAL(n) + lim) >= (uintptr_t)(63 + lim))) rt_range_error(w, RT_O(n));)
    (void)lim; (void)w;
    return RT_FIXVAL(n);
}
RT_INLINE intptr_t rt_fxashlw(intptr_t a, intptr_t n, const char *w) {
    intptr_t k = rt_fxshk(n, 0, w), r = (intptr_t)((uintptr_t)a << k);
    EXT_OVF((r >> k) != a, rt_fx_overflow(w, RT_O(a), RT_O(n)));
    return r;
}
RT_INLINE intptr_t rt_fxashrw(intptr_t a, intptr_t n, const char *w) { return (a >> rt_fxshk(n, 0, w)) & ~(intptr_t)1; }
RT_INLINE intptr_t rt_fxashw(intptr_t a, intptr_t n, const char *w) {
    intptr_t k = rt_fxshk(n, 62, w);
    return k >= 0 ? rt_fxashlw(a, n, w) : (a >> -k) & ~(intptr_t)1;
}
RT_INLINE obj rt_fxbitsetw(intptr_t k, intptr_t a, const char *w) { return RT_BOOL((a >> (rt_fxshk(k, 0, w) + 1)) & 1); }

/* bit-level operations on the untagged value: the 63-bit two's complement pattern */
static intptr_t ext_sx63(uint64_t u) { return (intptr_t)(u << 1) >> 1; } /* sign-extend from bit 62 */
static int ext_popcount(uint64_t u) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(u);
#else
    int n = 0;
    for (; u; u &= u - 1) n++;
    return n;
#endif
}
static int ext_clz(uint64_t u) { /* u != 0 */
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clzll(u);
#else
    int n = 0;
    for (; !(u >> 63); u <<= 1) n++;
    return n;
#endif
}
static uint64_t ext_mask(intptr_t w) { return w >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1); }
/* a field [s, e) of the 63-bit pattern: 0 <= s <= e <= 63 */
static void ext_field(obj s, obj e, intptr_t *ps, intptr_t *pe, const char *w) {
    rt_fxchk(s, w);
    rt_fxchk(e, w);
    *ps = RT_FIXVAL(s), *pe = RT_FIXVAL(e);
    RT_SAFE(if (*ps < 0 || *ps > 63) rt_range_error(w, s); if (*pe < *ps || *pe > 63) rt_range_error(w, e);)
}

/* fx+/carry, fx-/carry, fx* /carry (SRFI 143): s = i op j + k exactly (128-bit two's complement in
 * hi:lo), split as s = q * 2^63 + r with -2^62 <= r < 2^62 */
typedef struct { uint64_t hi, lo; } ext_u128;
static ext_u128 ext_s128(intptr_t v) { ext_u128 r; r.lo = (uint64_t)v; r.hi = v < 0 ? ~(uint64_t)0 : 0; return r; }
static ext_u128 ext_add128(ext_u128 a, ext_u128 b) { ext_u128 r; r.lo = a.lo + b.lo; r.hi = a.hi + b.hi + (r.lo < a.lo); return r; }
static ext_u128 ext_neg128(ext_u128 a) { ext_u128 r; r.lo = ~a.lo + 1; r.hi = ~a.hi + (r.lo == 0); return r; }
static ext_u128 ext_mul128(uint64_t a, uint64_t b) { /* unsigned 64 x 64 -> 128 */
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    ext_u128 r;
    r.lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}
static obj ext_carry(int op, obj i_, obj j_, obj k_, obj sel, const char *w) {
    intptr_t i, j, k, r;
    ext_u128 s;
    rt_fxchk(i_, w); rt_fxchk(j_, w); rt_fxchk(k_, w);
    i = RT_FIXVAL(i_), j = RT_FIXVAL(j_), k = RT_FIXVAL(k_);
    if (op == 2) {
        s = ext_mul128(i < 0 ? 0 - (uint64_t)i : (uint64_t)i, j < 0 ? 0 - (uint64_t)j : (uint64_t)j);
        if ((i < 0) != (j < 0)) s = ext_neg128(s);
        s = ext_add128(s, ext_s128(k));
    } else s = ext_add128(ext_add128(ext_s128(i), ext_s128(op ? -j : j)), ext_s128(op ? -k : k));
    r = ext_sx63(s.lo);
    if (sel == RT_FIX(0)) return RT_FIX(r);
    s = ext_add128(s, ext_neg128(ext_s128(r))); /* s - r = q * 2^63 */
    return RT_FIX((intptr_t)((s.hi << 1) | (s.lo >> 63)));
}

/* the generic (checked) forms */
#define EXT_FX1(name, who, expr) RT_FN obj name(obj x_) { intptr_t a; rt_fxchk(x_, who); a = (intptr_t)x_; return expr; }
#define EXT_FX2(name, who, expr) \
    RT_FN obj name(obj x_, obj y_) { intptr_t a, b; rt_fxchk(x_, who); rt_fxchk(y_, who); a = (intptr_t)x_, b = (intptr_t)y_; return expr; }
EXT_FX2(rt_fx_add, "fx+", RT_O(rt_fxaddw(a, b, "fx+")))
EXT_FX2(rt_fx_sub, "fx-", RT_O(rt_fxsubw(a, b, "fx-")))
EXT_FX2(rt_fx_mul, "fx*", RT_O(rt_fxmulw(a, b, "fx*")))
EXT_FX1(rt_fx_neg, "fxneg", RT_O(rt_fxnegw(a, "fxneg")))
EXT_FX1(rt_fx_abs, "fxabs", RT_O(rt_fxabsw(a, "fxabs")))
EXT_FX1(rt_fx_square, "fxsquare", RT_O(rt_fxmulw(a, a, "fxsquare")))
EXT_FX2(rt_fx_quotient, "fxquotient", RT_O(rt_fxquow(a, b, "fxquotient")))
EXT_FX2(rt_fx_remainder, "fxremainder", RT_O(rt_fxremw(a, b, "fxremainder")))
EXT_FX2(rt_fx_max, "fxmax", RT_O(a > b ? a : b))
EXT_FX2(rt_fx_min, "fxmin", RT_O(a < b ? a : b))
EXT_FX2(rt_fx_eq, "fx=?", RT_BOOL(a == b))
EXT_FX2(rt_fx_lt, "fx<?", RT_BOOL(a < b))
EXT_FX2(rt_fx_gt, "fx>?", RT_BOOL(a > b))
EXT_FX2(rt_fx_le, "fx<=?", RT_BOOL(a <= b))
EXT_FX2(rt_fx_ge, "fx>=?", RT_BOOL(a >= b))
EXT_FX1(rt_fx_zero_p, "fxzero?", RT_BOOL(a == 0))
EXT_FX1(rt_fx_positive_p, "fxpositive?", RT_BOOL(a > 0))
EXT_FX1(rt_fx_negative_p, "fxnegative?", RT_BOOL(a < 0))
EXT_FX1(rt_fx_odd_p, "fxodd?", RT_BOOL(a & 2))
EXT_FX1(rt_fx_even_p, "fxeven?", RT_BOOL(!(a & 2)))
EXT_FX1(rt_fx_not, "fxnot", RT_O(a ^ ~(intptr_t)1))
EXT_FX2(rt_fx_and, "fxand", RT_O(a & b))
EXT_FX2(rt_fx_ior, "fxior", RT_O(a | b))
EXT_FX2(rt_fx_xor, "fxxor", RT_O(a ^ b))
EXT_FX2(rt_fx_ash, "fxarithmetic-shift", RT_O(rt_fxashw(a, b, "fxarithmetic-shift")))
EXT_FX2(rt_fx_ashl, "fxarithmetic-shift-left", RT_O(rt_fxashlw(a, b, "fxarithmetic-shift-left")))
EXT_FX2(rt_fx_ashr, "fxarithmetic-shift-right", RT_O(rt_fxashrw(a, b, "fxarithmetic-shift-right")))
EXT_FX2(rt_fx_bit_set_p, "fxbit-set?", rt_fxbitsetw(a, b, "fxbit-set?"))
EXT_FX1(rt_fx_bit_count, "fxbit-count", RT_FIX(ext_popcount((uint64_t)(a < 0 ? ~RT_FIXVAL(a) : RT_FIXVAL(a)))))
EXT_FX1(rt_fx_length, "fxlength", RT_FIX(RT_FIXVAL(a) == 0 || RT_FIXVAL(a) == -1 ? 0 : 64 - ext_clz((uint64_t)(a < 0 ? ~RT_FIXVAL(a) : RT_FIXVAL(a)))))
EXT_FX1(rt_fx_first_set_bit, "fxfirst-set-bit", RT_FIX(a == 0 ? -1 : 63 - ext_clz((uint64_t)RT_FIXVAL(a) & (0 - (uint64_t)RT_FIXVAL(a)))))
RT_FN obj rt_fx_if(obj m, obj i, obj j) {
    rt_fxchk(m, "fxif"); rt_fxchk(i, "fxif"); rt_fxchk(j, "fxif");
    return RT_O(((intptr_t)m & (intptr_t)i) | (~(intptr_t)m & (intptr_t)j));
}
RT_FN obj rt_fx_copy_bit(obj k, obj i, obj b) {
    intptr_t n;
    rt_fxchk(k, "fxcopy-bit"); rt_fxchk(i, "fxcopy-bit");
    n = rt_fxshk((intptr_t)k, 0, "fxcopy-bit");
    RT_CHECK(b == RT_TRUE || b == RT_FALSE, "fxcopy-bit", "boolean", b);
    if (b == RT_TRUE) return RT_FIX(ext_sx63((uint64_t)RT_FIXVAL(i) | (uint64_t)1 << n));
    return RT_FIX(ext_sx63((uint64_t)RT_FIXVAL(i) & ~((uint64_t)1 << n)));
}
RT_FN obj rt_fx_bit_field(obj i, obj s, obj e) {
    intptr_t a, b;
    rt_fxchk(i, "fxbit-field");
    ext_field(s, e, &a, &b, "fxbit-field");
    RT_SAFE(if (b - a > 62) rt_range_error("fxbit-field", e);)
    return RT_FIX((intptr_t)(((uint64_t)RT_FIXVAL(i) >> a) & ext_mask(b - a)));
}
RT_FN obj rt_fx_bit_field_rotate(obj i, obj c, obj s, obj e) {
    intptr_t a, b, w, k;
    uint64_t u, f, m;
    rt_fxchk(i, "fxbit-field-rotate");
    rt_fxchk(c, "fxbit-field-rotate");
    ext_field(s, e, &a, &b, "fxbit-field-rotate");
    w = b - a;
    if (w == 0) return i;
    k = RT_FIXVAL(c) % w;
    if (k < 0) k += w;
    m = ext_mask(w);
    u = (uint64_t)RT_FIXVAL(i);
    f = (u >> a) & m;
    f = k ? ((f << k) | (f >> (w - k))) & m : f;
    return RT_FIX(ext_sx63((u & ~(m << a)) | (f << a)));
}
RT_FN obj rt_fx_bit_field_reverse(obj i, obj s, obj e) {
    intptr_t a, b, k;
    uint64_t u, f, r = 0;
    rt_fxchk(i, "fxbit-field-reverse");
    ext_field(s, e, &a, &b, "fxbit-field-reverse");
    u = (uint64_t)RT_FIXVAL(i);
    f = (u >> a) & ext_mask(b - a);
    for (k = 0; k < b - a; k++) r |= ((f >> k) & 1) << (b - a - 1 - k);
    return RT_FIX(ext_sx63((u & ~(ext_mask(b - a) << a)) | (r << a)));
}
RT_FN obj rt_fx_add_carry(obj i, obj j, obj k, obj sel) { return ext_carry(0, i, j, k, sel, "fx+/carry"); }
RT_FN obj rt_fx_sub_carry(obj i, obj j, obj k, obj sel) { return ext_carry(1, i, j, k, sel, "fx-/carry"); }
RT_FN obj rt_fx_mul_carry(obj i, obj j, obj k, obj sel) { return ext_carry(2, i, j, k, sel, "fx*/carry"); }

/* ---- flonums ---- */
RT_INLINE void rt_flchk(obj x, const char *who) { RT_SAFE(if (RT_UNLIKELY(!rt_is_flo(x))) rt_type_error(who, "flonum", x);) (void)x; (void)who; }
static uint64_t ext_bits(double d) { union { double d; uint64_t u; } x; x.d = d; return x.u; }
static double ext_dbl(uint64_t u) { union { double d; uint64_t u; } x; x.u = u; return x.d; }
#define EXT_SIGN ((uint64_t)1 << 63)
#define EXT_2P52 4503599627370496.0
RT_INLINE double rt_flcopysign(double x, double y) { return ext_dbl((ext_bits(x) & ~EXT_SIGN) | (ext_bits(y) & EXT_SIGN)); }
RT_INLINE intptr_t rt_flsignbit(double x) { return (intptr_t)(ext_bits(x) >> 63); }
RT_INLINE double rt_fltrunc(double x) { return rt_flabs(x) < EXT_2P52 ? rt_flcopysign((double)(int64_t)x, x) : x; }
RT_INLINE double rt_flfloor(double x) { double t = rt_fltrunc(x); return t > x ? t - 1.0 : t; }
RT_INLINE double rt_flceil(double x) { double t = rt_fltrunc(x); return t < x ? t + 1.0 : t; }
RT_INLINE double rt_flround(double x) { /* to nearest, ties to even (the default rounding of + and -) */
    double a = rt_flabs(x);
    return a < EXT_2P52 ? rt_flcopysign((a + EXT_2P52) - EXT_2P52, x) : x;
}
/* flmax / flmin: a NaN argument is ignored (C99 fmax / fmin); -0.0 < 0.0 */
RT_INLINE double rt_flmax2(double a, double b) { return a > b || b != b || (a == b && !rt_flsignbit(a)) ? a : b; }
RT_INLINE double rt_flmin2(double a, double b) { return a < b || b != b || (a == b && rt_flsignbit(a)) ? a : b; }
RT_FN double rt_flnext(double x, double y) { /* nextafter */
    uint64_t u;
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    if (x == 0) return rt_flcopysign(ext_dbl(1), y);
    u = ext_bits(x);
    return ext_dbl((x < y) == (x > 0) ? u + 1 : u - 1);
}
RT_FN double rt_flldexp(double x, intptr_t n) { /* x * 2^n, rounded once */
    if (n > 2200) n = 2200;
    if (n < -2200) n = -2200;
    while (n > 1023) { x *= 0x1p1023; n -= 1023; }
    while (n < -1022) { x *= 0x1p-969; n += 969; } /* 2^-1022 * 2^53: stays normal until the last step */
    return x * ext_dbl((uint64_t)(n + 1023) << 52);
}
/* ilogb of a finite nonzero x */
static intptr_t ext_ilogb(double x) {
    uint64_t u = ext_bits(x) & ~EXT_SIGN;
    intptr_t e = (intptr_t)(u >> 52);
    return e ? e - 1023 : (63 - ext_clz(u)) - 1074;
}
#define EXT_ILOGB0 (-2147483647 - 1) /* fl-integer-exponent-zero */
#define EXT_ILOGBNAN (-2147483647)   /* fl-integer-exponent-nan */
RT_INLINE intptr_t rt_flilogb(double x) { return x != x ? EXT_ILOGBNAN : x == 0 ? EXT_ILOGB0 : x - x != 0 ? 2147483647 : ext_ilogb(x); }
RT_INLINE double rt_fllogb(double x) { return x != x ? x : x == 0 ? ext_dbl(0xFFF0000000000000u) : x - x != 0 ? rt_flabs(x) : (double)ext_ilogb(x); }

/* x = m * 2^e with m < 2^53 an integer (m = 0 for zero) */
static uint64_t ext_decomp(double x, intptr_t *e) {
    uint64_t u = ext_bits(x) & ~EXT_SIGN, ex = u >> 52, m = u & (((uint64_t)1 << 52) - 1);
    if (ex) { *e = (intptr_t)ex - 1075; return m | (uint64_t)1 << 52; }
    *e = -1074;
    return m;
}
/* fl+*: x * y + z rounded once. With a hardware fused multiply-add the builtin is one instruction;
 * otherwise exact integer arithmetic: the 106-bit product and z aligned in a 128-bit window (the
 * smaller operand shifted right with a sticky bit, i.e. rounded to odd), then one rounding. */
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__FP_FAST_FMA) || defined(__ARM_FEATURE_FMA) || defined(__FMA__))
#define EXT_HW_FMA 1
RT_FN double rt_flfma(double x, double y, double z) { return __builtin_fma(x, y, z); }
#else
static ext_u128 ext_shl128(ext_u128 a, int k) { /* 0 <= k < 128 */
    ext_u128 r;
    if (k == 0) return a;
    if (k >= 64) { r.hi = a.lo << (k - 64); r.lo = 0; return r; }
    r.hi = (a.hi << k) | (a.lo >> (64 - k));
    r.lo = a.lo << k;
    return r;
}
static ext_u128 ext_shr128s(ext_u128 a, intptr_t k) { /* right shift, OR of the lost bits into bit 0 */
    ext_u128 r;
    uint64_t lost;
    if (k <= 0) return a;
    if (k >= 128) { r.hi = 0; r.lo = (a.hi | a.lo) != 0; return r; }
    if (k >= 64) {
        lost = a.lo | (k > 64 ? a.hi << (128 - k) : 0);
        r.lo = k == 64 ? a.hi : a.hi >> (k - 64);
        r.hi = 0;
    } else {
        lost = a.lo << (64 - k);
        r.lo = (a.lo >> k) | (a.hi << (64 - k));
        r.hi = a.hi >> k;
    }
    r.lo |= lost != 0;
    return r;
}
static ext_u128 ext_shr128(ext_u128 a, int k) { /* logical, 0 <= k < 128 */
    ext_u128 r;
    if (k == 0) return a;
    if (k >= 64) { r.lo = a.hi >> (k - 64); r.hi = 0; return r; }
    r.lo = (a.lo >> k) | (a.hi << (64 - k));
    r.hi = a.hi >> k;
    return r;
}
static int ext_msb128(ext_u128 a) { return a.hi ? 127 - ext_clz(a.hi) : 63 - ext_clz(a.lo); }
static int ext_lt128(ext_u128 a, ext_u128 b) { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
RT_FN double rt_flfma(double x, double y, double z) {
    intptr_t ex, ey, ez, E1, E2, sh;
    uint64_t mx, my, mz, m;
    ext_u128 P, Z, S;
    int sp, sz, neg, k;
    if (x - x != 0 || y - y != 0 || x == 0 || y == 0) return x * y + z; /* inf, nan, exact zero product */
    if (z - z != 0) return z;                                       /* the exact product is finite */
    if (z == 0) { double p = x * y; return p == 0 ? p : p + z; }
    mx = ext_decomp(x, &ex), my = ext_decomp(y, &ey), mz = ext_decomp(z, &ez);
    sp = (x < 0) != (y < 0), sz = z < 0;
    P = ext_mul128(mx, my), Z.hi = 0, Z.lo = mz;
    E1 = ex + ey - (124 - ext_msb128(P)), P = ext_shl128(P, 124 - ext_msb128(P)); /* most significant bit at 124 */
    E2 = ez - (124 - ext_msb128(Z)), Z = ext_shl128(Z, 124 - ext_msb128(Z));
    if (E1 < E2 || (E1 == E2 && ext_lt128(P, Z))) { /* P: the larger magnitude */
        ext_u128 t = P; intptr_t te = E1; int ts = sp;
        P = Z, E1 = E2, sp = sz, Z = t, E2 = te, sz = ts;
    }
    Z = ext_shr128s(Z, E1 - E2);
    if (sp == sz) S = ext_add128(P, Z);
    else S = ext_add128(P, ext_neg128(Z));
    neg = sp;
    if (!S.hi && !S.lo) return 0.0;
    k = ext_msb128(S);
    sh = k - 52;
    if (sh < -1074 - E1) sh = -1074 - E1; /* subnormal result: the last bit is 2^-1074 */
    if (sh <= 0) m = ext_shl128(S, (int)-sh).lo;
    else if (sh > 127) m = 0; /* below half of the smallest subnormal */
    else { /* round to nearest, ties to even; S mod 2^sh against 2^(sh-1) */
        ext_u128 q = ext_shr128(S, (int)sh), rem = ext_add128(S, ext_neg128(ext_shl128(q, (int)sh))), half;
        half.hi = 0, half.lo = 1;
        half = ext_shl128(half, (int)sh - 1);
        m = q.lo;
        if (ext_lt128(half, rem) || (rem.hi == half.hi && rem.lo == half.lo && (m & 1))) m++;
    }
    return rt_flldexp(neg ? -(double)m : (double)m, E1 + sh);
}
#endif
/* exact fmod (nearest = 0) and IEEE remainder (nearest = 1); *q: the low bits of the quotient */
RT_FN double rt_flrem(double x, double y, int nearest, intptr_t *q) {
    intptr_t ex, ey, d;
    uint64_t mx, my, qu = 0;
    double ax = rt_flabs(x), ay = rt_flabs(y), r;
    *q = 0;
    if (x != x || y != y || y == 0 || ax - ax != 0) return (x * y) / (x * y);
    if (ay - ay != 0) return x;
    if (ax < ay) r = ax;
    else {
        mx = ext_decomp(ax, &ex), my = ext_decomp(ay, &ey);
        while (mx < (uint64_t)1 << 52) mx <<= 1, ex--; /* normalise subnormals */
        while (my < (uint64_t)1 << 52) my <<= 1, ey--;
        for (d = ex - ey;; d--) {
            qu <<= 1;
            if (mx >= my) mx -= my, qu |= 1;
            if (d == 0) break;
            mx <<= 1;
        }
        r = rt_flldexp((double)mx, ey);
    }
    if (nearest && (r + r > ay || (r + r == ay && (qu & 1)))) r -= ay, qu++;
    qu &= 0x3FFFFFFF;
    *q = (x < 0) != (y < 0) ? -(intptr_t)qu : (intptr_t)qu;
    return x < 0 ? -r : r;
}
RT_INLINE double rt_flfmod(double x, double y) { intptr_t q; return rt_flrem(x, y, 0, &q); }
RT_INLINE double rt_flremq(double x, double y) { intptr_t q; return rt_flrem(x, y, 1, &q); }
RT_INLINE intptr_t rt_flremqbits(double x, double y) { intptr_t q; rt_flrem(x, y, 1, &q); return q; }

#define EXT_FL1(name, who, expr) RT_FN obj name(obj x_) { double a; rt_flchk(x_, who); a = RT_FLO(x_); return expr; }
#define EXT_FL2(name, who, expr) \
    RT_FN obj name(obj x_, obj y_) { double a, b; rt_flchk(x_, who); rt_flchk(y_, who); a = RT_FLO(x_), b = RT_FLO(y_); return expr; }
#define EXT_MK(e) rt_make_flonum(e)
RT_FN obj rt_flonum_p(obj x) { return RT_BOOL(rt_is_flo(x)); }
EXT_FL2(rt_fl_add, "fl+", EXT_MK(a + b))
EXT_FL2(rt_fl_sub, "fl-", EXT_MK(a - b))
EXT_FL2(rt_fl_mul, "fl*", EXT_MK(a * b))
EXT_FL2(rt_fl_div, "fl/", EXT_MK(a / b))
RT_FN obj rt_fl_fma(obj x, obj y, obj z) {
    rt_flchk(x, "fl+*"); rt_flchk(y, "fl+*"); rt_flchk(z, "fl+*");
    return EXT_MK(rt_flfma(RT_FLO(x), RT_FLO(y), RT_FLO(z)));
}
EXT_FL2(rt_fl_max, "flmax", EXT_MK(rt_flmax2(a, b)))
EXT_FL2(rt_fl_min, "flmin", EXT_MK(rt_flmin2(a, b)))
EXT_FL2(rt_fl_eq, "fl=?", RT_BOOL(a == b))
EXT_FL2(rt_fl_lt, "fl<?", RT_BOOL(a < b))
EXT_FL2(rt_fl_gt, "fl>?", RT_BOOL(a > b))
EXT_FL2(rt_fl_le, "fl<=?", RT_BOOL(a <= b))
EXT_FL2(rt_fl_ge, "fl>=?", RT_BOOL(a >= b))
EXT_FL1(rt_fl_abs, "flabs", EXT_MK(rt_flabs(a)))
EXT_FL1(rt_fl_floor, "flfloor", EXT_MK(rt_flfloor(a)))
EXT_FL1(rt_fl_ceiling, "flceiling", EXT_MK(rt_flceil(a)))
EXT_FL1(rt_fl_round, "flround", EXT_MK(rt_flround(a)))
EXT_FL1(rt_fl_truncate, "fltruncate", EXT_MK(rt_fltrunc(a)))
EXT_FL1(rt_fl_sqrt, "flsqrt", EXT_MK(rt_sys_sqrt(a)))
EXT_FL1(rt_fl_exp, "flexp", EXT_MK(rt_sys_exp(a)))
EXT_FL1(rt_fl_log, "fllog", EXT_MK(rt_sys_log(a)))
EXT_FL1(rt_fl_sin, "flsin", EXT_MK(rt_sys_sin(a)))
EXT_FL1(rt_fl_cos, "flcos", EXT_MK(rt_sys_cos(a)))
EXT_FL1(rt_fl_tan, "fltan", EXT_MK(rt_sys_tan(a)))
EXT_FL1(rt_fl_asin, "flasin", EXT_MK(rt_sys_asin(a)))
EXT_FL1(rt_fl_acos, "flacos", EXT_MK(rt_sys_acos(a)))
EXT_FL1(rt_fl_atan, "flatan", EXT_MK(rt_sys_atan2(a, 1.0)))
EXT_FL2(rt_fl_atan2, "flatan", EXT_MK(rt_sys_atan2(a, b)))
EXT_FL2(rt_fl_expt, "flexpt", EXT_MK(rt_sys_pow(a, b)))
EXT_FL2(rt_fl_copysign, "flcopysign", EXT_MK(rt_flcopysign(a, b)))
EXT_FL2(rt_fl_adjacent, "fladjacent", EXT_MK(rt_flnext(a, b)))
EXT_FL1(rt_fl_sign_bit, "flsign-bit", RT_FIX(rt_flsignbit(a)))
EXT_FL1(rt_fl_exponent, "flexponent", EXT_MK(rt_fllogb(a)))
EXT_FL1(rt_fl_integer_exponent, "flinteger-exponent", RT_FIX(rt_flilogb(a)))
EXT_FL2(rt_fl_remainder, "flremainder", EXT_MK(rt_flfmod(a, b)))
EXT_FL2(rt_fl_remquo, "flremquo", EXT_MK(rt_flremq(a, b)))
EXT_FL2(rt_fl_remquo_bits, "flremquo", RT_FIX(rt_flremqbits(a, b)))
RT_FN obj rt_make_flonum_e(obj x, obj n) {
    rt_flchk(x, "make-flonum");
    rt_fxchk(n, "make-flonum");
    return EXT_MK(rt_flldexp(RT_FLO(x), RT_FIXVAL(n)));
}
RT_FN obj rt_fl_fast_fma(void) {
#ifdef EXT_HW_FMA
    return RT_TRUE;
#else
    return RT_FALSE;
#endif
}

/* ---- f64vectors ---- */
RT_INLINE void rt_f64chk(obj v, const char *who) { RT_CHECK(rt_is_type(v, RT_T_F64VECTOR), who, "f64vector", v); (void)v; (void)who; }
RT_INLINE double rt_f64num(obj x, const char *who) { RT_CHECK(rt_is_num(x), who, "number", x); (void)who; return rt_num_dbl(x); }
RT_INLINE double rt_f64ref(obj v, intptr_t k) { /* proven f64vector and fixnum index (tagged) */
    RT_INDEX((uintptr_t)RT_FIXVAL(k) >= (uintptr_t)RT_HLEN(v) >> 3, "f64vector-ref", RT_O(k));
    return RT_F64(v)[RT_FIXVAL(k)];
}
RT_INLINE obj rt_f64set(obj v, intptr_t k, double d) {
    RT_SAFE(if (RT_UNLIKELY(RT_HFLAGS(v) & RT_F_RO)) rt_type_error("f64vector-set!", "mutable f64vector", v);)
    RT_INDEX((uintptr_t)RT_FIXVAL(k) >= (uintptr_t)RT_HLEN(v) >> 3, "f64vector-set!", RT_O(k));
    RT_F64(v)[RT_FIXVAL(k)] = d;
    return RT_UNSPEC;
}
static obj ext_f64n(intptr_t n) { return RT_TAGPTR(rt_alloc(RT_HDR(RT_T_F64VECTOR, 0, 8 * n), sizeof(uintptr_t) + 8 * (size_t)n)); }
RT_FN obj rt_make_f64vector(obj k, obj fill) {
    intptr_t n, i;
    double d;
    obj v;
    RT_CHECK(rt_is_fix(k) && RT_FIXVAL(k) >= 0 && RT_FIXVAL(k) < ((intptr_t)1 << 40), "make-f64vector", "valid length", k);
    d = rt_f64num(fill, "make-f64vector");
    n = RT_FIXVAL(k);
    v = ext_f64n(n);
    for (i = 0; i < n; i++) RT_F64(v)[i] = d;
    return v;
}
RT_FN obj rt_f64vector_p(obj x) { return RT_BOOL(rt_is_type(x, RT_T_F64VECTOR)); }
RT_FN obj rt_f64vector_length(obj v) { rt_f64chk(v, "f64vector-length"); return RT_FIX(RT_HLEN(v) >> 3); }
RT_FN obj rt_f64vector_ref(obj v, obj k) {
    rt_f64chk(v, "f64vector-ref");
    rt_fxchk(k, "f64vector-ref");
    return rt_make_flonum(rt_f64ref(v, (intptr_t)k));
}
RT_FN obj rt_f64vector_set(obj v, obj k, obj x) {
    rt_f64chk(v, "f64vector-set!");
    rt_fxchk(k, "f64vector-set!");
    return rt_f64set(v, (intptr_t)k, rt_f64num(x, "f64vector-set!"));
}
RT_FN obj rt_f64vector_to_list(obj v) {
    obj l = RT_NIL;
    intptr_t i;
    rt_f64chk(v, "f64vector->list");
    for (i = (RT_HLEN(v) >> 3) - 1; i >= 0; i--) l = rt_cons(rt_make_flonum(RT_F64(v)[i]), l);
    return l;
}
RT_FN obj rt_list_to_f64vector(obj l) {
    intptr_t n = rt_list_length(l, "list->f64vector"), i;
    obj v;
    for (v = l; v != RT_NIL; v = RT_CDR(v)) rt_f64num(RT_CAR(v), "list->f64vector");
    v = ext_f64n(n);
    for (i = 0; i < n; i++, l = RT_CDR(l)) RT_F64(v)[i] = rt_num_dbl(RT_CAR(l));
    return v;
}

/* ---- (shikra assume): with --unsafe a failed assumption is unreachable (the fact is trusted) ---- */
#if defined(__GNUC__) || defined(__clang__)
#define RT_UNREACHABLE() __builtin_unreachable()
#else
#define RT_UNREACHABLE() ((void)0)
#endif
RT_FN obj rt_assume_failed(obj messages) { rt_error("assume", "assumption violated", messages); }

/* ---- define-c (shikra ffi): argument and result conversions, checked unless --unsafe ---- */
static RT_UNUSED RT_NORETURN void ext_range(const char *who, const char *what, obj x) { rt_error(who, what, rt_cons(x, RT_NIL)); }
RT_FN long rt_ffi_long(obj x, const char *who) { rt_fxchk(x, who); return (long)RT_FIXVAL(x); }
RT_FN int rt_ffi_int(obj x, const char *who) {
    rt_fxchk(x, who);
    RT_SAFE(if (RT_FIXVAL(x) < -2147483647 - 1 || RT_FIXVAL(x) > 2147483647) ext_range(who, "argument out of range for int", x);)
    return (int)RT_FIXVAL(x);
}
RT_FN double rt_ffi_dbl(obj x, const char *who) { RT_CHECK(rt_is_num(x), who, "number", x); (void)who; return rt_num_dbl(x); }
RT_FN char rt_ffi_char(obj x, const char *who) {
    RT_CHECK(rt_is_char(x), who, "character", x);
    (void)who;
    RT_SAFE(if (RT_CHARVAL(x) > 255) ext_range(who, "argument out of range for char", x);)
    return (char)RT_CHARVAL(x);
}
RT_FN int rt_ffi_bool(obj x, const char *who) { RT_CHECK(x == RT_TRUE || x == RT_FALSE, who, "boolean", x); (void)who; return x == RT_TRUE; }
RT_FN const char *rt_ffi_str(obj x, const char *who) { RT_CHECK(rt_is_string(x), who, "string", x); (void)who; return RT_STR(x)->buf; }
RT_FN void *rt_ffi_ptr(obj x, const char *who) {
    rt_fxchk(x, who);
    RT_SAFE(if (RT_FIXVAL(x) < 0) ext_range(who, "argument out of range for pointer", x);)
    return (void *)(uintptr_t)RT_FIXVAL(x);
}
RT_FN obj rt_ffi_rlong(long r, const char *who) {
    RT_SAFE(if ((intptr_t)r < RT_FIX_MIN || (intptr_t)r > RT_FIX_MAX) rt_error(who, "result out of fixnum range", RT_NIL);)
    (void)who;
    return RT_FIX((intptr_t)r);
}
RT_FN obj rt_ffi_rptr(void *p, const char *who) {
    RT_SAFE(if ((uintptr_t)p > (uintptr_t)RT_FIX_MAX) rt_error(who, "pointer result out of fixnum range", RT_NIL);)
    (void)who;
    return RT_FIX((intptr_t)(uintptr_t)p);
}
RT_FN obj rt_ffi_rstr(const char *s) { return s ? rt_cstring(s) : RT_FALSE; }
#endif /* RT_HAVE_EXT */
