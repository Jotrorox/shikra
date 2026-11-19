/* rt_math.c -- the libm functions of the platform layer without libm (M6; DESIGN.md "M6 Linux
 * as built"): rt_sys_exp log sin cos tan asin acos atan2 pow sqrt for (scheme inexact) and
 * flonum expt. Used wherever the runtime does not take libm (everything but macOS), so a
 * static Linux program and the shikra tool on Linux compute the same results.
 *
 * Own code, written for this project: argument reduction (Cody-Waite for exp/log and moderate
 * trig arguments, Payne-Hanek with 1280 bits of 2/pi for large ones), Taylor and atanh series
 * with exact rational coefficients, and double-double (hi + lo) arithmetic where the last bits
 * matter (Dekker's exact product, Knuth's two-sum). Constants: tools/mathconst.py (exact
 * integer arithmetic). Measured error against macOS libm: tests/unit/math_unit.c. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif
#if !defined(RT_SYS_LIBM) || defined(RT_MATH_TEST)
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF /* a fused a*b+c would break the exact-product identities */
#endif
#ifndef RT_MATH
#define RT_MATH(f) rt_sys_##f /* the unit test renames them to compare with the host libm */
#endif

typedef union { double d; uint64_t u; } mt_bits;
static uint64_t mt_u(double x) { mt_bits b; b.d = x; return b.u; }
static double mt_d(uint64_t u) { mt_bits b; b.u = u; return b.d; }
#define MT_INF mt_d(0x7ff0000000000000u)
#define MT_NAN mt_d(0x7ff8000000000000u)
#define MT_ABSU(x) (mt_u(x) & 0x7fffffffffffffffu)

/* Double-double helpers: a*b == *p + *e exactly (Dekker), a+b == *s + *e exactly (Knuth). */
static void mt_mul(double a, double b, double *p, double *e) {
    double c = 134217729.0 * a, ah = c - (c - a), al = a - ah, bh, bl;
    c = 134217729.0 * b, bh = c - (c - b), bl = b - bh;
    *p = a * b;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}
static void mt_add(double a, double b, double *s, double *e) {
    double t;
    *s = a + b, t = *s - a;
    *e = (a - (*s - t)) + (b - t);
}
static double mt_scale(double x, int k) { /* x * 2^k, rounded once */
    if (k > 1023) {
        x *= 0x1p1023, k -= 1023;
        if (k > 1023) { x *= 0x1p1023, k -= 1023; if (k > 1023) k = 1023; }
    } else if (k < -1022) {
        x *= 0x1p-969, k += 969;
        if (k < -1022) { x *= 0x1p-969, k += 969; if (k < -1022) k = -1022; }
    }
    return x * mt_d((uint64_t)(0x3ff + k) << 52);
}

#define MT_LN2_H 0x1.62e42fee00000p-1 /* 32 bits: k * MT_LN2_H is exact for |k| < 2^21 */
#define MT_LN2_L 0x1.a39ef35793c76p-33
#define MT_PIO2_H 0x1.921fb54442d18p+0
#define MT_PIO2_L 0x1.1a62633145c07p-54
#define MT_PI_H 0x1.921fb54442d18p+1
#define MT_PI_L 0x1.1a62633145c07p-53

/* exp(x + xl), |xl| tiny against x: x = k ln2 + r, |r| <= ln2/2, exp(r) by Taylor to r^14
 * with 1 + r + r^2/2 summed exactly. */
static double mt_exp(double x, double xl) {
    double kd, r, rl, s, e, s2, e2, zh, zl, p;
    int k;
    if (x != x) return x;
    if (x > 709.79) return MT_INF;
    if (x < -745.2) return 0.0;
    kd = x * 0x1.71547652b82fep+0;
    k = (int)(kd < 0 ? kd - 0.5 : kd + 0.5);
    kd = k;
    mt_add(x - kd * MT_LN2_H, xl - kd * MT_LN2_L, &r, &rl);
    p = 1.0 / 87178291200.0;
    p = p * r + 1.0 / 6227020800.0;
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    mt_mul(r, r, &zh, &zl);
    mt_add(1.0, r, &s, &e);
    mt_add(s, 0.5 * zh, &s2, &e2);
    return mt_scale(s2 + (e + e2 + 0.5 * zl + r * zh * p + rl * (1.0 + r)), k);
}

/* log(x) for finite x > 0 as hi + *lo (about 2^-66 relative): x = 2^k m, m in [sqrt(1/2),
 * sqrt(2)), log m = 2 atanh(s), s = (m-1)/(m+1) in double-double, series to s^27. */
static double mt_log(double x, double *lo) {
    uint64_t u = mt_u(x);
    int k = 0;
    double m, f, uh, ul, sh, sl, p, pe, zh, zl, ch, cl, th, tl, t5, a, ae, b, be, c, kd;
    if (u < 0x0010000000000000u) x *= 0x1p54, u = mt_u(x), k = -54; /* subnormal */
    k += (int)(u >> 52) - 1023;
    m = mt_d((u & 0x000fffffffffffffu) | 0x3ff0000000000000u);
    if (m > 0x1.6a09e667f3bcdp+0) m *= 0.5, k++;
    f = m - 1.0; /* exact */
    mt_add(2.0, f, &uh, &ul);
    sh = f / uh;
    mt_mul(sh, uh, &p, &pe);
    sl = (((f - p) - pe) - sh * ul) / uh;
    mt_mul(sh, sh, &zh, &zl);
    zl += 2.0 * sh * sl;
    mt_mul(sh, zh, &ch, &cl); /* s^3 */
    cl += sh * zl + sl * zh;
    mt_mul(ch, 0x1.5555555555555p-1, &th, &tl); /* (2/3) s^3 */
    tl += cl * 0x1.5555555555555p-1 + ch * 0x1.5555555555555p-55;
    p = 2.0 / 27;
    p = p * zh + 2.0 / 25;
    p = p * zh + 2.0 / 23;
    p = p * zh + 2.0 / 21;
    p = p * zh + 2.0 / 19;
    p = p * zh + 2.0 / 17;
    p = p * zh + 2.0 / 15;
    p = p * zh + 2.0 / 13;
    p = p * zh + 2.0 / 11;
    p = p * zh + 2.0 / 9;
    p = p * zh + 2.0 / 7;
    p = p * zh + 2.0 / 5;
    t5 = ch * zh * p;
    kd = k;
    mt_add(kd * MT_LN2_H, 2.0 * sh, &a, &ae);
    mt_add(a, th, &b, &be);
    c = ae + be + 2.0 * sl + tl + t5 + kd * MT_LN2_L;
    a = b + c;
    *lo = c - (a - b);
    return a;
}

/* ---- trigonometric argument reduction: x = n pi/2 + (*rh + *rl), |r| <= ~pi/4 ---- */
static const uint32_t mt_2opi[40] = { /* 2/pi: bits 1..1280 after the binary point */
    0xa2f9836e, 0x4e441529, 0xfc2757d1, 0xf534ddc0, 0xdb629599, 0x3c439041, 0xfe5163ab, 0xdebbc561,
    0xb7246e3a, 0x424dd2e0, 0x06492eea, 0x09d1921c, 0xfe1deb1c, 0xb129a73e, 0xe88235f5, 0x2ebb4484,
    0xe99c7026, 0xb45f7e41, 0x3991d639, 0x835339f4, 0x9c845f8b, 0xbdf9283b, 0x1ff897ff, 0xde05980f,
    0xef2f118b, 0x5a0a6d1f, 0x6d367ecf, 0x27cb09b7, 0x4f463f66, 0x9e5fea2d, 0x7527bac7, 0xebe5f17b,
    0x3d0739f7, 0x8a5292ea, 0x6bfb5fb1, 0x1f8d5d08, 0x56033046, 0xfc7b6bab, 0xf0cfbc20, 0x9af4361d};
static uint64_t mt_bits64(const uint32_t *p, int pos) { /* bits pos..pos+63 of little-endian p */
    int w = pos >> 5, s = pos & 31;
    uint64_t lo = p[w] | (uint64_t)p[w + 1] << 32;
    return s ? lo >> s | (uint64_t)p[w + 2] << (64 - s) : lo;
}
/* Payne-Hanek for |x| >= pi/4: x = M 2^E; the bits of 2/pi that matter mod 4 times M, exactly. */
static int mt_reduce_big(double ax, double *rh, double *rl) {
    uint64_t u = mt_u(ax), M = (u & 0x000fffffffffffffu) | 0x0010000000000000u, f1, f0;
    int E = (int)(u >> 52) - 1075, i0 = E - 1 > 1 ? E - 1 : 1, o = i0 - 1, sh, i, j, n;
    uint32_t W[6], P[10] = {0}, m[2];
    double c1, c2, c3, fh, fl, ph, pl;
    for (i = 0; i < 6; i++) { /* W[0] least significant: 192 bits from bit i0 of 2/pi */
        int w = (o >> 5) + 5 - i, s = o & 31;
        W[i] = s ? mt_2opi[w] << s | mt_2opi[w + 1] >> (32 - s) : mt_2opi[w];
    }
    m[0] = (uint32_t)M, m[1] = (uint32_t)(M >> 32);
    for (j = 0; j < 2; j++) {
        uint64_t c = 0;
        for (i = 0; i < 6; i++) {
            uint64_t t = (uint64_t)W[i] * m[j] + P[i + j] + c;
            P[i + j] = (uint32_t)t, c = t >> 32;
        }
        P[6 + j] = (uint32_t)c;
    }
    sh = i0 + 191 - E; /* position of the binary point in P */
    n = (int)(mt_bits64(P, sh) & 3);
    f1 = mt_bits64(P, sh - 64), f0 = mt_bits64(P, sh - 128);
    if (f1 >> 63) { /* fraction >= 1/2: take r = fraction - 1 (two's complement, 128 bits) */
        n = (n + 1) & 3;
        f1 = ~f1, f0 = ~f0 + 1;
        if (!f0) f1++;
        c1 = -1.0;
    } else c1 = 1.0;
    c3 = c1 * (double)(f0 & 0x3fffff) * 0x1p-128;
    c2 = c1 * (double)((f1 & 0x7ff) << 42 | f0 >> 22) * 0x1p-106;
    c1 = c1 * (double)(f1 >> 11) * 0x1p-53;
    mt_add(c1, c2, &fh, &fl);
    fl += c3;
    mt_mul(fh, MT_PIO2_H, &ph, &pl);
    pl += fh * MT_PIO2_L + fl * MT_PIO2_H;
    *rh = ph + pl;
    *rl = pl - (*rh - ph);
    return n;
}
static int mt_reduce(double x, double *rh, double *rl) {
    double ax = x < 0 ? -x : x, kd, r1, w, r2, e2;
    int n;
    if (ax <= 0x1.921fb54442d18p-1) { *rh = x, *rl = 0; return 0; }
    if (ax < 0x1p20) { /* Cody-Waite: pi/2 in 33 + 33 + 53 bits */
        kd = ax * 0x1.45f306dc9c883p-1;
        n = (int)(kd + 0.5), kd = n;
        r1 = ax - kd * 0x1.921fb54400000p+0; /* exact */
        w = kd * 0x1.0b4611a600000p-34;      /* exact */
        mt_add(r1, -w, &r2, &e2);
        if (r2 > 0x1p-26 || r2 < -0x1p-26) {
            e2 -= kd * 0x1.3198a2e037073p-69;
            *rh = r2 + e2;
            *rl = e2 - (*rh - r2);
            goto done;
        }
    }
    n = mt_reduce_big(ax, rh, rl);
done:
    if (x < 0) *rh = -*rh, *rl = -*rl, n = -n;
    return n & 3;
}

/* sin and cos of r = rh + rl, |r| <= ~pi/4, as hi + *lo: Taylor to r^19 and r^18. */
static double mt_ksin(double rh, double rl, double *lo) { /* r - r^3/6 in double-double */
    double zh, zl, ch, cl, d, dl, p, pe, s, e, t, h;
    mt_mul(rh, rh, &zh, &zl);
    mt_mul(rh, zh, &ch, &cl);
    cl += rh * zl;
    d = ch / 6.0;
    mt_mul(d, 6.0, &p, &pe);
    dl = (((ch - p) - pe) + cl) / 6.0;
    p = -1.0 / 121645100408832000.0;
    p = p * zh + 1.0 / 355687428096000.0;
    p = p * zh - 1.0 / 1307674368000.0;
    p = p * zh + 1.0 / 6227020800.0;
    p = p * zh - 1.0 / 39916800.0;
    p = p * zh + 1.0 / 362880.0;
    p = p * zh - 1.0 / 5040.0;
    p = p * zh + 1.0 / 120.0;
    mt_add(rh, -d, &s, &e);
    t = e - dl + ch * zh * p + rl * (1.0 - 0.5 * zh);
    h = s + t;
    *lo = t - (h - s);
    return h;
}
static double mt_kcos(double rh, double rl, double *lo) {
    double zh, zl, w, e, p, t, h;
    mt_mul(rh, rh, &zh, &zl);
    p = 1.0 / 6402373705728000.0;
    p = p * zh - 1.0 / 20922789888000.0;
    p = p * zh + 1.0 / 87178291200.0;
    p = p * zh - 1.0 / 479001600.0;
    p = p * zh + 1.0 / 3628800.0;
    p = p * zh - 1.0 / 40320.0;
    p = p * zh + 1.0 / 720.0;
    p = p * zh - 1.0 / 24.0;
    w = 1.0 - 0.5 * zh;
    e = (1.0 - w) - 0.5 * zh;
    t = e - 0.5 * zl - rh * rl - zh * zh * p;
    h = w + t;
    *lo = t - (h - w);
    return h;
}
/* (ah + al) / (bh + bl) as hi + *lo */
static double mt_div(double ah, double al, double bh, double bl, double *lo) {
    double q = ah / bh, p, pe, h;
    mt_mul(q, bh, &p, &pe);
    p = ((((ah - p) - pe) + al) - q * bl) / bh;
    h = q + p;
    *lo = p - (h - q);
    return h;
}
static double mt_trig(double x, int which) { /* 0 sin, 1 cos, 2 tan */
    double rh, rl, s, sl, c, cl;
    int n;
    if (MT_ABSU(x) >= 0x7ff0000000000000u) return x - x; /* inf or NaN: NaN */
    if (MT_ABSU(x) < 0x3e50000000000000u) return which == 1 ? 1.0 : x; /* |x| < 2^-26 */
    n = mt_reduce(x, &rh, &rl);
    if (which == 2) {
        s = mt_ksin(rh, rl, &sl), c = mt_kcos(rh, rl, &cl);
        return n & 1 ? -mt_div(c, cl, s, sl, &sl) : mt_div(s, sl, c, cl, &sl);
    }
    n = (n + which) & 3;
    s = n & 1 ? mt_kcos(rh, rl, &sl) : mt_ksin(rh, rl, &sl);
    return n & 2 ? -s : s;
}

/* atan(t + tl) for t >= 0 as hi + *lo: atan(t) = pi/2 - atan(1/t) for t > 1, then
 * atan(t) = atan(c) + atan((t-c)/(1+tc)) with c = j/8 nearest to t, |u| <= 1/16, series to u^17. */
static const double mt_atanj[9][2] = {{0, 0},
    {0x1.fd5ba9aac2f6ep-4, -0x1.cd37686760c17p-59}, {0x1.f5b75f92c80ddp-3, 0x1.8ab6e3cf7afbdp-57},
    {0x1.6f61941e4def1p-2, -0x1.c63aae6f6e918p-56}, {0x1.dac670561bb4fp-2, 0x1.a2b7f222f65e2p-56},
    {0x1.1e00babdefeb4p-1, -0x1.928df287a668fp-58}, {0x1.4978fa3269ee1p-1, 0x1.2419a87f2a458p-56},
    {0x1.700a7c5784634p-1, -0x1.8c34d25aadef6p-56}, {0x1.921fb54442d18p-1, 0x1.1a62633145c07p-55}};
static double mt_atan(double t, double tl, double *lo) {
    double y, p, pe, u, ul, c, dh, dl, z, s, e, h, tail;
    int j, inv = t > 1.0;
    if (t == MT_INF) { *lo = MT_PIO2_L; return MT_PIO2_H; }
    if (inv) {
        y = 1.0 / t;
        mt_mul(y, t, &p, &pe);
        tl = (((1.0 - p) - pe) - y * tl) / t;
        t = y;
    }
    j = (int)(t * 8.0 + 0.5);
    if (j == 0) u = t, ul = tl;
    else {
        c = j * 0.125;
        mt_mul(t, c, &p, &pe);
        mt_add(1.0, p, &dh, &dl);
        dl += pe + tl * c;
        u = (t - c) / dh; /* t - c is exact */
        mt_mul(u, dh, &p, &pe);
        ul = (((((t - c) - p) - pe) + tl) - u * dl) / dh;
    }
    z = u * u;
    p = 1.0 / 17;
    p = p * z - 1.0 / 15;
    p = p * z + 1.0 / 13;
    p = p * z - 1.0 / 11;
    p = p * z + 1.0 / 9;
    p = p * z - 1.0 / 7;
    p = p * z + 1.0 / 5;
    p = p * z - 1.0 / 3;
    mt_add(mt_atanj[j][0], u, &s, &e);
    tail = e + mt_atanj[j][1] + ul * (1.0 - z) + u * z * p;
    if (inv) { /* pi/2 - (s + tail) */
        mt_add(MT_PIO2_H, -s, &h, &e);
        tail = e + (MT_PIO2_L - tail);
        s = h;
    }
    h = s + tail;
    *lo = tail - (h - s);
    return h;
}

RT_FN double RT_MATH(exp)(double x) { return mt_exp(x, 0.0); }
RT_FN double RT_MATH(log)(double x) {
    double lo;
    if (x != x || x == MT_INF) return x;
    if (x == 0) return -MT_INF;
    if (x < 0) return MT_NAN;
    return mt_log(x, &lo);
}
RT_FN double RT_MATH(sin)(double x) { return mt_trig(x, 0); }
RT_FN double RT_MATH(cos)(double x) { return mt_trig(x, 1); }
RT_FN double RT_MATH(tan)(double x) { return mt_trig(x, 2); }

RT_FN double RT_MATH(sqrt)(double x) {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    __asm__("sqrtsd %1, %0" : "=x"(x) : "x"(x)); /* IEEE sqrt is correctly rounded */
    return x;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    __asm__("fsqrt %d0, %d1" : "=w"(x) : "w"(x));
    return x;
#else /* portable: Newton from a halved exponent, then one exact-residual correction */
    double y, p, e;
    int i;
    if (!(x > 0) || x == MT_INF) return x < 0 ? MT_NAN : x;
    y = mt_d((mt_u(x) >> 1) + 0x1ff8000000000000u);
    for (i = 0; i < 6; i++) y = 0.5 * (y + x / y);
    mt_mul(y, y, &p, &e);
    return y + ((x - p) - e) / (2.0 * y);
#endif
}

/* asin and acos through atan: d = sqrt(1 - x^2) in double-double. */
static double mt_cosof(double ax, double *dl) {
    double a, bh, bl, qh, ql, e, p, pe, sh;
    if (ax >= 0.5) {
        a = 1.0 - ax; /* exact */
        mt_add(1.0, ax, &bh, &bl);
        mt_mul(a, bh, &qh, &ql);
        ql += a * bl;
    } else {
        mt_mul(ax, ax, &p, &pe);
        mt_add(1.0, -p, &qh, &e);
        ql = e - pe;
    }
    sh = RT_MATH(sqrt)(qh);
    if (sh == 0) { *dl = 0; return 0; }
    mt_mul(sh, sh, &p, &pe);
    *dl = (((qh - p) - pe) + ql) / (2.0 * sh);
    return sh;
}
RT_FN double RT_MATH(asin)(double x) {
    double ax = x < 0 ? -x : x, d, dl, z, zl, lo, r;
    if (!(ax <= 1.0)) return MT_NAN;
    if (ax < 0x1p-26) return x;
    d = mt_cosof(ax, &dl);
    if (d == 0) r = MT_PIO2_H;
    else {
        z = mt_div(ax, 0, d, dl, &zl);
        r = mt_atan(z, zl, &lo);
    }
    return x < 0 ? -r : r;
}
RT_FN double RT_MATH(acos)(double x) {
    double ax = x < 0 ? -x : x, d, dl, z, zl, lo, r, h, e;
    if (!(ax <= 1.0)) return MT_NAN;
    if (ax < 0x1p-57) return MT_PIO2_H;
    d = mt_cosof(ax, &dl);
    z = mt_div(d, dl, ax, 0, &zl);
    r = mt_atan(z, zl, &lo);
    if (x > 0) return r;
    mt_add(MT_PI_H, -r, &h, &e);
    return h + (e + (MT_PI_L - lo));
}
RT_FN double RT_MATH(atan2)(double y, double x) {
    double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, z, p, pe, zl, h, e, lo, r;
    int ex, ey, neg = (int)(mt_u(y) >> 63), xneg = (int)(mt_u(x) >> 63);
    if (x != x || y != y) return x + y;
    if (ay == 0) r = xneg ? MT_PI_H : 0.0;
    else if (ax == 0 || ay == MT_INF) r = ax == MT_INF ? (xneg ? 3 * 0x1.921fb54442d18p-1 : 0x1.921fb54442d18p-1) : MT_PIO2_H;
    else if (ax == MT_INF) r = xneg ? MT_PI_H : 0.0;
    else {
        ex = (int)(mt_u(ax) >> 52), ey = (int)(mt_u(ay) >> 52);
        if (ey - ex > 60) r = MT_PIO2_H;
        else if (ex - ey > 60) r = xneg ? MT_PI_H : ay / ax;
        else {
            if (ax > 0x1p500 || ay > 0x1p500) ax *= 0x1p-600, ay *= 0x1p-600;
            else if (ax < 0x1p-500 || ay < 0x1p-500) ax *= 0x1p600, ay *= 0x1p600;
            z = ay / ax;
            mt_mul(z, ax, &p, &pe);
            zl = ((ay - p) - pe) / ax;
            r = mt_atan(z, zl, &lo);
            if (xneg) {
                mt_add(MT_PI_H, -r, &h, &e);
                r = h + (e + (MT_PI_L - lo));
            }
        }
    }
    return neg ? -r : r;
}

/* pow = exp(y log|x|) with log in double-double; C99 Annex F special cases. */
static int mt_int(double y) { /* 0 not an integer, 1 odd, 2 even */
    double a = y < 0 ? -y : y;
    if (!(a < 0x1p53)) return a == MT_INF || a != a ? 0 : 2;
    if ((double)(int64_t)a != a) return 0;
    return (int64_t)a & 1 ? 1 : 2;
}
RT_FN double RT_MATH(pow)(double x, double y) {
    double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, lh, ll, ph, pl, r;
    int yi, neg = 0;
    if (y == 0 || x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;
    yi = mt_int(y);
    if (ay == MT_INF) {
        if (ax == 1.0) return 1.0;
        return (ax > 1.0) == (y > 0) ? MT_INF : 0.0;
    }
    if (x == 0 || ax == MT_INF) {
        r = (x == 0) == (y < 0) ? MT_INF : 0.0;
        return yi == 1 && mt_u(x) >> 63 ? -r : r;
    }
    if (x < 0) {
        if (!yi) return MT_NAN;
        neg = yi == 1;
    }
    lh = mt_log(ax, &ll);
    ph = y * lh;
    if (lh == 0) r = 1.0; /* x = -1 (y even); y may be too large to split */
    else if (ph > 1000 || ph < -1000) r = ph > 0 ? MT_INF : 0.0;
    else {
        mt_mul(y, lh, &ph, &pl);
        r = mt_exp(ph, pl + y * ll);
    }
    return neg ? -r : r;
}
#endif
