/* math_unit.c -- src/rt_math.c (the runtime's own libm, M6) against the host libm.
 * Built by tests/run.sh (unit): cc -std=c99 -O2 -Isrc tests/unit/math_unit.c -lm
 * The functions are compiled under other names (RT_MATH) next to <math.h>. For each function:
 * >= 10^5 random arguments per range across magnitudes; the report gives the largest observed
 * difference from the host libm in ulps of the libm result and how many results differ at all.
 * Fails if any difference exceeds MAXULP or a special case (signed zeros, infinities, NaN,
 * domain errors, exact identities) is not exactly right. */
#define RT_MATH_TEST 1
#define RT_MATH(f) mt_test_##f
#include "rt.h"
#include "rt_math.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXULP 2.0
#define N 100000
static int fails, dump; /* -dump: print differing arguments (for an exact-arithmetic check) */
static uint64_t seed = 0x9e3779b97f4a7c15u;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static double uni(double a, double b) { return a + (b - a) * (double)(rnd() >> 11) * 0x1p-53; }
static double logu(double lo2, double hi2) { return exp2(uni(lo2, hi2)) * (1.0 + uni(0, 1) * 0x1p-30); }
static double sgn(double x) { return rnd() & 1 ? -x : x; }

static double ulps(double got, double want) {
    double u;
    if (isnan(want)) return isnan(got) ? 0 : 1e9;
    if (isinf(want) || isinf(got)) return got == want ? 0 : 1e9;
    if (got == want) return 0;
    u = want == 0 ? 0x1p-1074 : fabs(nextafter(want, INFINITY) - want);
    if (fabs(want) < 0x1p-1022) u = 0x1p-1074;
    return fabs(got - want) / u;
}
typedef struct { const char *name; double max, maxarg, maxarg2; long n, diff; } stat;
static void note(stat *s, double got, double want, double a, double b) {
    double e = ulps(got, want);
    s->n++;
    if (e > 0) s->diff++;
    if (dump && e > 0 && (s->diff % 50 == 1 || e > 1.5))
        printf("%s %a %a %a %a\n", s->name, a, b, got, want);
    if (e > s->max) s->max = e, s->maxarg = a, s->maxarg2 = b;
}
static void report(stat *s) {
    printf("math_unit: %-6s %8ld args  max %.2f ulp  differ %.4f%%  (worst at %.17g", s->name, s->n, s->max,
           100.0 * (double)s->diff / (double)s->n, s->maxarg);
    if (s->maxarg2 == s->maxarg2 && s->maxarg2 != 0) printf(", %.17g", s->maxarg2);
    printf(")\n");
    if (s->max > MAXULP) { fails++; printf("FAIL: %s error above %.1f ulp\n", s->name, MAXULP); }
}

typedef double (*f1)(double);
static void run1(const char *name, f1 mine, f1 ref, double (*gen)(int), int ranges) {
    stat s;
    int r;
    long i;
    memset(&s, 0, sizeof s);
    s.name = name;
    for (r = 0; r < ranges; r++)
        for (i = 0; i < N; i++) {
            double x = gen(r);
            note(&s, mine(x), ref(x), x, 0);
        }
    report(&s);
}
static double g_exp(int r) { return r == 0 ? uni(-745.2, 709.8) : r == 1 ? sgn(logu(-60, 9.4)) : uni(-1, 1); }
static double g_log(int r) {
    uint64_t u;
    if (r == 1) return 1.0 + uni(-0.3, 0.42);
    if (r == 2) return 1.0 + sgn(logu(-52, -10));
    do u = rnd() >> 1; while ((u >> 52) == 0x7ff);
    return mt_d(u); /* every positive finite double, subnormals included */
}
static double g_trig(int r) {
    if (r == 0) return uni(-10, 10);
    if (r == 1) return sgn(logu(-30, 30));
    if (r == 2) return sgn(logu(30, 1023));
    return (double)(int)uni(1, 2e6) * 1.5707963267948966 * (1 + uni(-1, 1) * 0x1p-50); /* near n pi/2 */
}
static double g_asin(int r) {
    if (r == 0) return uni(-1, 1);
    if (r == 1) return sgn(1.0 - logu(-53, -1));
    return sgn(logu(-30, -1));
}

int main(int argc, char **argv) {
    stat s;
    long i;
    int r;
    dump = argc > 1 && !strcmp(argv[1], "-dump");
    run1("exp", mt_test_exp, exp, g_exp, 3);
    run1("log", mt_test_log, log, g_log, 3);
    run1("sin", mt_test_sin, sin, g_trig, 4);
    run1("cos", mt_test_cos, cos, g_trig, 4);
    run1("tan", mt_test_tan, tan, g_trig, 4);
    run1("asin", mt_test_asin, asin, g_asin, 3);
    run1("acos", mt_test_acos, acos, g_asin, 3);
    run1("sqrt", mt_test_sqrt, sqrt, g_log, 3);
    memset(&s, 0, sizeof s);
    s.name = "atan2";
    for (r = 0; r < 3; r++)
        for (i = 0; i < N; i++) {
            double y, x;
            if (r == 0) y = sgn(logu(-60, 60)), x = sgn(logu(-60, 60));
            else if (r == 1) y = sgn(logu(-1074, 1023)), x = sgn(logu(-1074, 1023));
            else y = uni(-4, 4), x = 1.0; /* one-argument atan as the emitter calls it */
            note(&s, mt_test_atan2(y, x), atan2(y, x), y, x);
        }
    report(&s);
    memset(&s, 0, sizeof s);
    s.name = "pow";
    for (r = 0; r < 5; r++)
        for (i = 0; i < N; i++) {
            double x, y;
            if (r == 0) x = logu(-20, 20), y = uni(-50, 50);
            else if (r == 1) x = logu(-1, 1), y = uni(-700, 700); /* large y log x */
            else if (r == 2) x = 1.0 + sgn(logu(-50, -5)), y = sgn(logu(5, 55));
            else if (r == 3) x = -logu(-10, 10), y = (double)(int)uni(-60, 60); /* negative base */
            else x = logu(-1074, 1023), y = uni(-2, 2);
            note(&s, mt_test_pow(x, y), pow(x, y), x, y);
        }
    report(&s);

    /* special cases: bit-exact (signed zeros, NaN as NaN) */
#define SAME(got, want) do { double g_ = (got), w_ = (want); \
        if (!(isnan(w_) ? isnan(g_) : mt_u(g_) == mt_u(w_))) { fails++; \
            printf("FAIL %s:%d: %s = %.17g, expected %.17g\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
    {
        static const double sp[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, -3.0, 0x1p-1074, -0x1p-1074,
                                    0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023, 709.78, -745.13, 1e300, -1e300};
        double v[sizeof sp / sizeof sp[0] + 3];
        size_t n = sizeof sp / sizeof sp[0], a, b;
        memcpy(v, sp, sizeof sp);
        v[n++] = INFINITY, v[n++] = -INFINITY, v[n++] = NAN;
        for (a = 0; a < n; a++) {
            double x = v[a];
            if (isinf(x) || isnan(x) || x == 0 || fabs(x) == 1 || fabs(x) < 1e-300) {
                SAME(mt_test_exp(x), exp(x)); SAME(mt_test_log(x), log(x)); SAME(mt_test_sin(x), sin(x));
                SAME(mt_test_cos(x), cos(x)); SAME(mt_test_tan(x), tan(x)); SAME(mt_test_asin(x), asin(x));
                SAME(mt_test_acos(x), acos(x)); SAME(mt_test_sqrt(x), sqrt(x));
            }
            for (b = 0; b < n; b++) {
                double y = v[b];
                if (isinf(x) || isnan(x) || x == 0 || isinf(y) || isnan(y) || y == 0 || fabs(x) == 1) {
                    SAME(mt_test_pow(x, y), pow(x, y));
                    SAME(mt_test_atan2(x, y), atan2(x, y));
                }
            }
        }
        SAME(mt_test_asin(1.5), NAN); SAME(mt_test_acos(-1.0000001), NAN); SAME(mt_test_log(-1.0), NAN);
        SAME(mt_test_pow(-8.0, 1.0 / 3), NAN); SAME(mt_test_sqrt(-2.0), NAN);
        /* exact identities the R7RS suite and tests/cases rely on */
        SAME(mt_test_exp(1.0), 2.718281828459045); SAME(4 * mt_test_atan2(1.0, 1.0), 3.141592653589793);
        SAME(mt_test_pow(2.0, 3.0), 8.0); SAME(mt_test_pow(4.0, 0.5), 2.0); SAME(mt_test_pow(10.0, 2.0), 100.0);
        SAME(mt_test_pow(2.0, -3.0), 0.125); SAME(mt_test_sqrt(16.0), 4.0); SAME(mt_test_sqrt(2.0), 1.4142135623730951);
        SAME(mt_test_log(8.0) / mt_test_log(2.0), 3.0); SAME(mt_test_log(4096.0) / mt_test_log(2.0), 12.0);
        SAME(mt_test_log(100.0) / mt_test_log(10.0), log(100.0) / log(10.0));
        SAME(mt_test_log(mt_test_exp(1.0)), 1.0); SAME(mt_test_acos(-1.0), 3.141592653589793);
        SAME(mt_test_asin(1.0), 1.5707963267948966); SAME(mt_test_atan2(1.0, 0.0), 1.5707963267948966);
        SAME(mt_test_atan2(1.0, -1.0), atan2(1.0, -1.0)); SAME(mt_test_atan2(-0.0, -1.0), -3.141592653589793);
        SAME(mt_test_sin(0x1p1023), sin(0x1p1023)); SAME(mt_test_cos(1e22), cos(1e22));
    }
    printf("math_unit: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
