#!/usr/bin/env python3
# tests/cases/inc/fixnum_ref.py -- writes tests/cases/ext_fixnum_ref.out: the SRFI 143 results for
# the values and operations of tests/cases/ext_fixnum_ref.scm, computed independently with Python's
# unbounded integers (fx-width 63: fixnums are -2^62 .. 2^62-1). Run once; the output is committed.
W = 63
HI = 2**62 - 1
LO = -2**62
V = [0, 1, -1, 2, -2, 3, 5, 7, 255, -256, 12345678901, -98765432109, HI, LO, HI - 1, LO + 1,
     2**31, -2**31, 2**61, -2**61]
C = [0, 1, 5, 31, 61, 62, -1, -62]
S = [0, 1, -1, 7, HI, LO, 2**40, -2**40]
def fx(x): return "overflow" if x < LO or x > HI else str(x)
def sx(u): u &= 2**W - 1; return u - 2**W if u >> (W - 1) else u
def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q
out = []
def line(op, args, r): out.append("%s %s => %s" % (op, " ".join(map(str, args)), r))
for a in V:
    for b in V:
        line("fx+", (a, b), fx(a + b)); line("fx-", (a, b), fx(a - b)); line("fx*", (a, b), fx(a * b))
        line("fxand", (a, b), a & b); line("fxior", (a, b), a | b); line("fxxor", (a, b), a ^ b)
        if b == 0: line("fxquotient", (a, b), "error"); line("fxremainder", (a, b), "error")
        else:
            q = trunc_div(a, b); line("fxquotient", (a, b), fx(q)); line("fxremainder", (a, b), a - b * q)
        line("fx<?", (a, b), "#t" if a < b else "#f")
    line("fxneg", (a,), fx(-a)); line("fxabs", (a,), fx(abs(a))); line("fxsquare", (a,), fx(a * a)); line("fxnot", (a,), ~a)
    line("fxbit-count", (a,), bin(a if a >= 0 else ~a).count("1"))
    line("fxlength", (a,), (a if a >= 0 else ~a).bit_length())
    line("fxfirst-set-bit", (a,), -1 if a == 0 else (a & -a).bit_length() - 1)
    for n in C:
        if abs(n) >= W: r = "error"
        elif n >= 0: r = fx(a << n)
        else: r = a >> -n
        line("fxarithmetic-shift", (a, n), r)
        if 0 <= n < W: line("fxarithmetic-shift-right", (a, n), a >> n); line("fxarithmetic-shift-left", (a, n), fx(a << n))
        if 0 <= n < W:
            line("fxbit-set?", (n, a), "#t" if (a >> n) & 1 else "#f")
            line("fxcopy-bit", (n, a, "#t"), sx(a | (1 << n))); line("fxcopy-bit", (n, a, "#f"), sx(a & ~(1 << n)))
    for (s, e) in [(0, 0), (0, 1), (0, 8), (3, 17), (0, 62), (1, 63), (40, 63), (62, 63), (10, 10)]:
        line("fxbit-field", (a, s, e), (a >> s) & ((1 << (e - s)) - 1))
        w = e - s
        u = a & (2**W - 1)
        f = (u >> s) & ((1 << w) - 1)
        for cnt in [0, 1, 3, -2, 70]:
            if w == 0: r = a
            else:
                k = cnt % w
                g = ((f << k) | (f >> (w - k))) & ((1 << w) - 1)
                r = sx((u & ~(((1 << w) - 1) << s)) | (g << s))
            line("fxbit-field-rotate", (a, cnt, s, e), r)
        g = int(format(f, "0%db" % w)[::-1], 2) if w else 0
        line("fxbit-field-reverse", (a, s, e), sx((u & ~(((1 << w) - 1) << s)) | (g << s)))
for i in S:
    for j in S:
        for k in S:
            for op, s in (("fx+/carry", i + j + k), ("fx-/carry", i - j - k), ("fx*/carry", i * j + k)):
                r = ((s + 2**(W - 1)) % 2**W) - 2**(W - 1)
                line(op, (i, j, k), "%d %d" % (r, (s - r) >> W))
for m, a, b in [(0, 5, 9), (-1, 5, 9), (0xF0F0, 0x1234, -1), (LO, HI, LO), (12345, -1, 0)]:
    line("fxif", (m, a, b), (m & a) | (~m & b))
print("\n".join(out))
