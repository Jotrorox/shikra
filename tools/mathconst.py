#!/usr/bin/env python3
# mathconst.py -- prints the constants of src/rt_math.c (DESIGN.md "M6 Linux as built").
# Not part of the build (the output is pasted into rt_math.c); kept so the tables can be
# checked. Everything is exact integer arithmetic: pi from Machin's formula, ln 2 from the
# series of atanh(1/3), atan(j/8) from its Taylor series, each to PREC bits; a double is
# printed as a C99 hexadecimal literal.
import math
from fractions import Fraction

PREC = 1400
ONE = 1 << PREC

def atan_inv(n, hyperbolic=False):  # atan(1/n) * ONE, or atanh(1/n) * ONE
    s, t, k, sign = 0, ONE // n, 1, 1
    while t:
        s += sign * (t // k)
        t //= n * n
        k += 2
        sign = sign if hyperbolic else -sign
    return s

def atan_frac(p, q):  # atan(p/q) * ONE for p/q <= 1/2, by the Taylor series in exact rationals
    x = Fraction(p, q)
    s, term, k = Fraction(0), x, 1
    while abs(term) * ONE > Fraction(1, 1 << 8):
        s += term / k
        term *= -x * x
        k += 2
    return int(s * ONE)

PI = 4 * (4 * atan_inv(5) - atan_inv(239))
LN2 = 2 * atan_inv(3, True)  # ln 2 = 2 atanh(1/3)

def hexd(v):  # v is a Fraction or int scaled by ONE -> nearest double, as a hex literal
    return float(Fraction(v, ONE)).hex()

def split(v, bits):  # leading `bits` significant bits of v (scaled), then the rest
    x = Fraction(v, ONE)
    e = math.floor(math.log2(x))
    q = 2 ** (bits - 1 - e)
    hi = Fraction(math.floor(x * q), q)
    return hi, x - hi

def show(name, x):
    print("%s = %s" % (name, float(x).hex()))

# pi/2 in three parts for Cody-Waite reduction: 33 + 33 + 53 bits
hi1, r = split(PI // 2, 33)
show("PIO2_1", hi1)
e = math.floor(math.log2(r)); q = 2 ** (32 - e); hi2 = Fraction(math.floor(r * q), q); r -= hi2
show("PIO2_2", hi2)
show("PIO2_3", r)
# pi/2, pi, pi/4 as double-double
for name, v in (("PIO2", PI // 2), ("PI", PI), ("PIO4", PI // 4)):
    x = Fraction(v, ONE); h = Fraction(float(x)); show(name + "_H", h); show(name + "_L", x - h)
# ln 2: hi with 32 bits (k * hi exact for |k| < 2^21), lo the rest
h, l = split(LN2, 32)
show("LN2_H", h); show("LN2_L", l)
x = Fraction(LN2, ONE); hd = Fraction(float(x)); show("LN2_D", hd); show("LN2_DL", x - hd)
show("INVLN2", Fraction(1) / x)
# atan(j/8), j = 1..8, as double-double
for j in range(1, 9):
    if j == 8:
        v = Fraction(PI // 4, ONE)
    elif j <= 4:
        v = Fraction(atan_frac(j, 8), ONE)
    else:  # atan(x) = pi/4 - atan((1-x)/(1+x)) for x > 1/2
        v = Fraction(PI // 4, ONE) - Fraction(atan_frac(8 - j, 8 + j), ONE)
    h = Fraction(float(v)); print("ATAN_%d = %s, %s" % (j, float(h).hex(), float(v - h).hex()))
# 2/pi bits: 40 words of 32 bits after the binary point
TWO_PI_BITS = (2 * ONE * ONE) // PI  # (2/pi) * 2^PREC
words = []
for i in range(40):
    words.append((TWO_PI_BITS >> (PREC - 32 * (i + 1))) & 0xFFFFFFFF)
print("TWO_OVER_PI = {" + ", ".join("0x%08x" % w for w in words) + "}")
