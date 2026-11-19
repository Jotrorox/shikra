#!/usr/bin/env python3
# tests/cases/inc/bignum_ref.py -- writes the expected outputs of the --bignum cases
# tests/cases/big_{arith,text,float,random,kmul,fixnum,unsafe}.out, computed independently with Python's
# unbounded integers and IEEE doubles (int -> float conversion and int / int are correctly rounded
# in Python, comparisons of int with float are exact). Run from anywhere; the outputs are committed:
#     python3 tests/cases/inc/bignum_ref.py
# Each Scheme program prints in a format reproduced here: (show x ...) writes each value and a
# space, then a newline; exact integers in decimal, booleans as #t / #f, symbols and strings as
# written. The error messages are the runtime's (constant text, not computed).
import math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.dirname(HERE)
HI, LO = 2**62 - 1, -2**62
M = 2**61 - 1  # digest modulus (a fixnum)


def w(x):
    if x is True: return "#t"
    if x is False: return "#f"
    if isinstance(x, str): return x
    return str(x)


def show(out, *xs): out.append("".join(w(x) + " " for x in xs))


def tq(a, b):  # truncate quotient
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def tr(a, b): return a - b * tq(a, b)


def flt(x):  # the nearest double, or +inf / -inf
    try: return float(x)
    except OverflowError: return math.inf if x > 0 else -math.inf


def fdiv(a, b):  # a / b correctly rounded, or +inf / -inf
    try: return a / b
    except OverflowError: return math.inf if (a < 0) == (b < 0) else -math.inf


def fdig(d):  # digest of a double (Scheme: (exact (floor (* d 1e6))) mod M), infinities by name
    if math.isinf(d): return "+inf" if d > 0 else "-inf"
    e = d * 1e6
    if math.isinf(e): return "+inf" if e > 0 else "-inf"
    return math.floor(e) % M


def write(name, lines):
    with open(os.path.join(CASES, name), "w") as f:
        f.write("\n".join(lines) + "\n")


# ---- big_arith ----
def arith():
    o = []
    for n in (20, 21, 25, 50, 100):
        show(o, "fact", n, math.factorial(n))
    f1000 = math.factorial(1000)
    show(o, "fact1000", len(str(f1000)), f1000 % 1000000007, f1000 % M)
    show(o, "fact1000/fact998", f1000 // math.factorial(998))
    a, b = 0, 1
    for i in range(1000): a, b = b, a + b
    show(o, "fib", 1000, a)
    for n in range(58, 67):
        p = 2**n
        show(o, "pow2", n, p, -p, p - 1, -p + 1, p * p)
    show(o, "pow3", 3**200)
    t = 3**10000
    show(o, "pow3-10000", len(str(t)), t % M)
    A = [0, 1, -1, 7, -7, HI, LO, HI + 1, LO - 1, 2**64 + 5, -(2**64) - 5, 10**30 + 7, -(10**30) - 7,
         3**100, -(3**100), 2**200 - 1]
    B = [1, -1, 3, -3, 1000000007, -1000000007, HI, LO, 2**64, -(2**64), 10**20 + 9, -(10**20) - 9, 3**60]
    for x in A:
        for y in B:
            q, r = tq(x, y), tr(x, y)
            show(o, "qr", x, y, q, r, x % y, x // y, y * q + r == x, (x * y) // y == x)
    for x, y in [(2**100, 2**80 * 3), (math.factorial(30), math.factorial(25) * 7), (0, 2**70), (-(2**90), 6**40),
                 (10**40, 10**41 + 1)]:
        show(o, "gcd-lcm", math.gcd(x, y), abs(x * y) // math.gcd(x, y) if x and y else 0)
    for k in (2**62, 2**64, 10**30, 3**101, 2**199 + 12345):
        for j in (k * k - 1, k * k, k * k + 1):
            s = math.isqrt(j)
            show(o, "isqrt", j, s, j - s * s)
    # boundaries at +-2^62: results normalise back to fixnums (eq? on fixnums compares words)
    show(o, "bound", HI + 1, LO - 1, True, True, True, True)
    show(o, "neg", -LO, abs(LO), -(-LO) == LO)
    show(o, "square", (2**40 + 3)**2, (-(2**70))**2)
    show(o, "eqv", True, True, False, True, True)
    show(o, "zero", False, True, False, True, True, False)
    show(o, "odd-even", True, False, False, True)
    show(o, "maxmin", 2**100, -(2**100), True)
    show(o, "expt", (-3)**41, (2**70)**3, 1, 1, -1, 0, 1)
    show(o, "exact-integer?", True, True, True, True, True, True, False)
    show(o, "rounding", 2**100, -(2**100), 2**100, 2**100, 2**100)
    show(o, "numden", 2**100, 1)
    for m in ["vector-ref: index out of range", "string-ref: index out of range", "make-vector: expected valid length",
              "integer->char: expected Unicode scalar value", "list-tail: index out of range", "expt: result too large",
              "exact-integer-sqrt: expected exact nonnegative integer", "bytevector-u8-ref: index out of range",
              "make-string: expected valid length"]:
        show(o, "error", '"' + m + '"')
    write("big_arith.out", o)


# ---- big_text: radix conversion, reader literals ----
def text():
    o = []
    vals = [2**62, -(2**62) - 1, 2**64 - 1, -(2**64), 10**30, 3**150, -(7**77), 2**521 - 1, int("1" * 120), 16**40 + 1]
    for v in vals:
        for r, fmt in ((2, "b"), (8, "o"), (10, "d"), (16, "x")):
            show(o, "radix", r, '"' + format(v, fmt) + '"', True)
    show(o, "lit", 0x123456789abcdef0123456789, -0b1010101010101010101010101010101010101010101010101010101010101010101,
         0o7777777777777777777777777, 123456789012345678901234567890, -123456789012345678901234567890)
    show(o, "lit0", 123456789012345678901, 123456789012345678901)
    show(o, "e-lit", 10**30, 1500, 12345678901234567890, -(10**25), 10**400 % M)
    show(o, "s->n", 10**30, 1500, False, False, 2**80, -1, -(2**80), 2**66 // 4, 0x10000000000000000 // 2)
    show(o, "ratio", fdig(10**20 / 3), fdig(-(2**70) / 7))
    show(o, "inexact-lit", fdig(float(2**70)), fdig(flt(10**400)))
    t = 10**9999 + 1
    s = str(t)
    u = str(3**20959)
    show(o, "big-print", len(s), '"' + s[:20] + '"', '"' + s[-20:] + '"', True, len(u), int(u[-12:]))
    show(o, "display", 2**100)
    write("big_text.out", o)


# ---- big_float: mixed comparisons, exact/inexact ----
def flo():
    o = []
    pts = [2**53, 2**53 + 1, 2**53 - 1, 2**63, 2**63 + 1, 2**63 - 1, 2**64, 10**300, 10**300 + 1, 2**1023 * 3 // 2,
           -(2**63) - 1, -(2**63), -(10**300)]
    dbls = [2.0**53, 2.0**53 + 2, 9223372036854775808.0, 9223372036854774784.0, 1.8446744073709552e19, 1e300,
            -9223372036854775808.0, -1e300, 1e308, math.inf, -math.inf, 0.5, -1e-300]
    for x in pts:
        show(o, "cmp", x, '"' + "".join(("<" if x < d else "=" if x == d else ">") for d in dbls) + '"')
    show(o, "nan", False, False, False, False)
    for x in [2**53 + 1, 2**53 + 3, 2**64 + 2**11, 2**64 + 2**11 + 1, 2**64 + 3 * 2**11, (2**54 + 1) * 2**100,
              (2**54 + 3) * 2**100, 2**1024 - 2**970, 2**1024 - 2**971, 2**1024 - 2**970 - 1, -(2**64) - 2**11, 3**500]:
        d = flt(x)
        show(o, "inexact", x, "inf" if math.isinf(d) else int(d))
    for d in [1e30, -1e30, 2.0**62, -(2.0**62), 2.0**63, 1.7976931348623157e308, 123456789012345680000.0, 4611686018427387904.0]:
        show(o, "exact", int(d), int(d) == int(d))
    show(o, "maxmin", int(float(2**100)), True, int(float(-(2**100))), True)
    show(o, "sqrt", 10**20, True, fdig(math.sqrt(2**101)), True)
    show(o, "div", 2**98, fdig(2**100 / 3), fdig(1 / 2**70), fdig((10**400 + 1) / 10**399))
    show(o, "integer?", True, True, True, True)
    show(o, "q-inexact", fdig(float(tq(2**70, 3))), fdig(float((2**70) % 7)), fdig(float(tq(int(1e30), 7))))
    show(o, "log", True, True)
    write("big_float.out", o)


# ---- big_random: differential corpus ----
def corpus(n):
    st = [12345]

    def rnd():
        st[0] = st[0] * 48271 % 2147483647
        return st[0]

    def rbits(k):
        r, b = 0, 0
        while b < k:
            r, b = r * 2147483648 + rnd(), b + 31
        return r // 2**(b - k)

    def operand():
        c = rnd() % 100
        if c < 8: v = 2**62 + rnd() % 5 - 2
        else: v = rbits(1 + rnd() % (64 if c < 30 else 256 if c < 65 else 1024 if c < 92 else 3000))
        return -v if rnd() % 2 else v

    o = []
    for i in range(n):
        a, b = operand(), operand()
        if b == 0: b = 1
        op = i % 18
        if op == 0: r = (a + b) % M
        elif op == 1: r = (a - b) % M
        elif op == 2: r = (a * b) % M
        elif op == 3: r = tq(a, b) % M
        elif op == 4: r = tr(a, b) % M
        elif op == 5: r = (a % b) % M
        elif op == 6: r = (a // b) % M
        elif op == 7: r = math.gcd(a, b) % M
        elif op == 8: r = (abs(a * b) // math.gcd(a, b) if a else 0) % M
        elif op == 9: r = w(a < b) + " " + w(a > b)
        elif op == 10: r = "#t #t #t"
        elif op == 11:
            e = rnd() % 7
            base = a % 2**300 if a >= 0 else -((-a) % 2**300)
            r = (base ** e) % M
        elif op == 12:
            s = math.isqrt(abs(a))
            r = "%d %d" % (s % M, (abs(a) - s * s) % M)
        elif op == 13:
            r = (a // b) % M if a % b == 0 else fdig(fdiv(a, b))
        elif op == 14: r = fdig(flt(a))
        elif op == 15: fb = flt(b); r = w(a < fb) + " " + w(a == flt(a))
        elif op == 16: s = format(a, "x"); r = "%d #t" % len(s)
        else: s = str(a); r = "%d #t" % len(s)
        o.append("%d %s" % (i, w(r)))
    write("big_random.out", o)


def fixnum():
    o = []
    show(o, "fixnum?", True, False, False, True, True)
    show(o, "fx+", "overflow", "overflow", "overflow", "overflow")
    show(o, "generic", HI + 1, LO - 1, HI * 2, -LO)
    write("big_fixnum.out", o)


def unsafe():
    o = []
    s = 0
    for i in range(1, 2000001): s += i * i * i
    show(o, "sum", HI + 1, LO - 1, HI * HI, s)
    acc = 1
    for i in range(1, 31): acc *= i
    show(o, "fact", acc, sum(range(0, 10**6 + 1)) * 2**40)
    show(o, "quotient", 2**62, -2**61, 2**62, 2**62, 2**62)
    write("big_unsafe.out", o)


# ---- big_kmul ----
def kmul():
    st = [777]

    def rnd():
        st[0] = st[0] * 48271 % 2147483647
        return st[0]

    def rbits(k):
        r, b = 0, 0
        while b < k:
            r, b = r * 2147483648 + rnd(), b + 31
        return r
    o = []
    for i in range(120):
        a = rbits(1200 + rnd() % 29000)
        b = rbits(1200 + rnd() % (29000 if i % 2 == 0 else 4000))
        if i % 3 == 0: a = -a
        c = rbits(1 + rnd() % 1000)
        p = a * b
        q, r = tq(p + c, b), tr(p + c, b)
        o.append("%d %d %d %d %d #t" % (i, p % M, a * a % M, q % M, r % M))
    write("big_kmul.out", o)


arith()
text()
flo()
corpus(4000)
kmul()
fixnum()
unsafe()
