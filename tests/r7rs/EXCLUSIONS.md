# Exclusions and rewrites

This file lists every place where `tests/r7rs/*.scm` departs from
`upstream/r7rs-tests.scm` (Chibi Scheme, commit `9b0c19a`). DESIGN.md §1
allows a departure only where an implementation restriction stated in
`docs/shikra-spec-0.1.tex` makes the upstream test inapplicable. Everything
else in upstream is in the per-library files, in upstream order and with
upstream text. Rewritten lines carry the comment
`; shikra: rewritten, see EXCLUSIONS.md`. "Line" means the line number in
`upstream/r7rs-tests.scm`.

Moving a test into another per-library file (for example `exp` into
`inexact.scm`, `string-ci=?` into `char.scm`, `display` into `write.scm`) is
not a change and is not listed here. Every per-library file imports only
`(scheme base)`, its own library and `(chibi test)`. The one exception is
`read.scm`, which also imports `(scheme write)` because upstream's
`test-numeric-syntax` macro both reads and writes.

## Counts

A *test form* is one upstream `test`, `test-values`, `test-error` or
`test-assert` form, or one use of upstream's helper macros
`test-numeric-syntax`, `test-write-syntax`, `test-read-error` or
`test-precision`. An *assertion* is one test as counted at run time. Each
`test-numeric-syntax` use makes 2 assertions, and each `test-precision` use
makes 1 or 2.

|                          | test forms | assertions at run time |
|--------------------------|-----------:|-----------------------:|
| upstream total           |       1126 |                   1236 |
| retained unchanged       |       1037 |                   1113 |
| rewritten                |          7 |                      7 |
| excluded                 |         82 |                    116 |
| retained total (all files) |     1044 |                   1120 |

The upstream assertion count (1236) is from running the unmodified upstream
file on Chibi master with Chibi's own `(chibi test)`. The retained count
(1120) is the sum of the final lines of the 12 programs below.

| file | library | test forms | assertions |
|------|---------|-----------:|-----------:|
| `base.scm` | `(scheme base)` | 658 | 658 |
| `char.scm` | `(scheme char)`, ASCII | 68 | 68 |
| `char-unicode.scm` | `(scheme char)`, needs `--unicode=full` | 46 | 46 |
| `inexact.scm` | `(scheme inexact)` | 41 | 41 |
| `lazy.scm` | `(scheme lazy)` | 12 | 12 |
| `case-lambda.scm` | `(scheme case-lambda)` | 13 | 13 |
| `file.scm` | `(scheme file)` | 4 | 4 |
| `read.scm` | `(scheme read)` | 145 | 210 |
| `write.scm` | `(scheme write)` | 46 | 57 |
| `process-context.scm` | `(scheme process-context)` | 4 | 4 |
| `time.scm` | `(scheme time)` | 4 | 4 |
| `eval.scm` | `(scheme eval)`, REPL only | 3 | 3 |

There is no `cxr.scm`, `load.scm` or `repl.scm`. Upstream has no test that
uses a `(scheme cxr)` procedure (it uses only `cadr` and `cddr`, which are in
`(scheme base)`) and no test of `load` or `interaction-environment`; its only
`interaction-environment` uses are inside a `#| ... |#` comment. No tests
were invented to fill these gaps.

## Rewritten (7)

### base.scm

| line | upstream | now | reason |
|-----:|----------|-----|--------|
| 916 | `(test 3/20 (/ 3 4 5))` | `(test 0.15 (/ 3 4 5))` | §3.2 "No exact rationals. `(/ 1 3)` returns the flonum 0.333…": `(/ 3 4)` is 0.75 and 0.75/5 is 0.15. |
| 917 | `(test 1/3 (/ 3))` | `(test 0.3333333333333333 (/ 3))` | §3.2, the spec's own example `(/ 1 3)`. |
| 922 | `(test 4611686018427387904 (/ -4611686018427387904 -1))` | `(test-error (/ -4611686018427387904 -1))` | §3.2 "Exact integers are 63-bit fixnums. Overflow raises an error and never wraps": -2^62 is the smallest fixnum, so 2^62 overflows. |
| 923 | `(test 4611686018427387904 (quotient -4611686018427387904 -1))` | `(test-error (quotient -4611686018427387904 -1))` | Same as line 922. |
| 959 | `(test 3 (numerator (/ 6 4)))` | `(test 3.0 (numerator (/ 6 4)))` | §3.2: `(/ 6 4)` is the flonum 1.5, and R7RS defines `(numerator 1.5)` as 3.0. |
| 960 | `(test 2 (denominator (/ 6 4)))` | `(test 2.0 (denominator (/ 6 4)))` | Same: `(denominator 1.5)` is 2.0. |

### inexact.scm

| line | upstream | now | reason |
|-----:|----------|-----|--------|
| 202 | `(test 1800/497 c)` | `(test 3.6217303822937628 c)` | §3.2: `(mean / /)` sums `(/ x)` over `(8 5 99 1 22)`. Shikra computes this in flonums, so the result is the flonum nearest 1800/497. Tests `a` (exact 27) and `b` (9.728) are unchanged. |

## Excluded (82 test forms)

### base.scm (30)

Exact integers are 63-bit fixnums (§3.2). These results do not fit, and
§3.2 says that "overflow raises an error". Each test is about something else
(`exact-integer-sqrt`, transitivity), so it is excluded rather than turned
into an overflow test:

| line | test | reason |
|-----:|------|--------|
| 215 | `(test '(815238614083298888 443242361398135744) (let*-values (((root rem) (exact-integer-sqrt (expt 2 119)))) (list root rem)))` | 2^119 is a bignum (§3.2). |
| 219 | `(test '(1152921504606846976 0) (let*-values (((root rem) (exact-integer-sqrt (expt 2 120)))) (list root rem)))` | 2^120 is a bignum (§3.2). |
| 223 | `(test '(1630477228166597776 1772969445592542976) (let*-values (((root rem) (exact-integer-sqrt (expt 2 121)))) (list root rem)))` | 2^121 is a bignum (§3.2). |
| 227 | `(test '(31622776601683793319 62545769258890964239) (let*-values (((root rem) (exact-integer-sqrt (expt 10 39)))) (list root rem)))` | 10^39 is a bignum (§3.2). |
| 231–233 | `(let*-values (((root rem) (exact-integer-sqrt (expt 2 140)))) (test 0 rem) (test (expt 2 140) (square root)))` (2 forms) | 2^140 is a bignum (§3.2). |
| 834–839 | `(let ((a (- (expt 2 1000) 1)) (b (inexact (expt 2 1000))) (c (+ (expt 2 1000) 1))) (test #t (if (and (= a b) (= b c)) (= a c) #t)))` | 2^1000 is a bignum (§3.2). The comment block above it (lines 825–833) was removed with it. |
| 849–857 | `(define single-float-epsilon ...)` `(let* ((a (/ 10.0 single-float-epsilon)) (j (exact a))) (test #t ...))` | `(exact a)` with a ≈ 1.1e-15 needs an exact rational result (§3.2 "No exact rationals"). The helper definition was used only here and was removed with the test. |

No complex numbers: `(scheme complex)` is omitted, and complex literals have
no meaning in Shikra (§3, §3.1 table, §3.4 "never sees ... exact-complex"):

| line | test |
|-----:|------|
| 763 | `(test #t (complex? 3+4i))` |
| 766 | `(test #t (real? -2.5+0i))` |
| 767 | `(test #f (real? -2.5+0.0i))` |
| 777 | `(test #t (integer? 3+0i))` |
| 808 | `(test #t (= 1 1.0 1.0+0.0i))` |
| 809 | `(test #f (= 1.0 1.0+1.0i))` |
| 861 | `(test #t (zero? 0.0+0.0i))` |
| 915 | `(test -3/2-i (- 3/2+i))` |

No exact rationals (§3.2). These tests need a non-integral exact rational,
written as a literal or produced as a result:

| line | test |
|-----:|------|
| 775 | `(test #t (rational? 6/10))` |
| 787 | `(test #f (exact-integer? 32/5))` |
| 914 | `(test -3/2 (- 3/2))` |
| 977 | `(test 4 (round 7/2))` |
| 979 | `(test 1 (round 7/10))` |
| 980 | `(test -4 (round -7/2))` |
| 982 | `(test -1 (round -7/10))` |
| 984 | `(test 1/3 (rationalize (exact .3) 1/10))` |
| 985 | `(test #i1/3 (rationalize .3 1/10))` (the argument `1/10` is a non-integral exact literal) |
| 1039 | `(test 1/1024 (expt 2 -10))` |
| 1040 | `(test 1/1180591620717411303424 (expt 2 -70))` |

Lines 1039–1040: the exact answer is a rational. The spec states the flonum
result only for `/`, so these were not rewritten. If the spec is extended to
"any exact operation with a non-integral result returns a flonum", restore
them as `(test 0.0009765625 (expt 2 -10))` and
`(test 8.470329472543003e-22 (expt 2 -70))`.

Rejected at compile time (§11.3 Diagnostics: "The wrong number of arguments
to a known procedure" and "A value of a provably wrong type"). Inside
`test-error` these would stop `base.scm` from compiling at all:

| line | test | reason |
|-----:|------|--------|
| 1661 | `(test-error (apply +))` | `apply` needs at least 2 arguments: an arity error at a known call. |
| 1662 | `(test-error (apply + 3))` | The last argument to `apply` must be a list, and fixnum 3 provably is not. |
| 1663 | `(test-error (apply + 3 4))` | Same as line 1662. |

Line 1664, `(test-error (apply + '(2 3 . 4)))`, is **kept**. The type
lattice (§5.2) has only "pair" and cannot prove that a pair is an improper
list, so this one is a run-time error.

### inexact.scm (11)

All are complex numbers (§3, `(scheme complex)` omitted):

| line | test |
|-----:|------|
| 791 | `(test #f (finite? 3.0+inf.0i))` |
| 796 | `(test #t (infinite? 3.0+inf.0i))` |
| 800 | `(test #t (nan? +nan.0+5.0i))` |
| 801 | `(test #t (nan? 1.0+nan.0i))` |
| 802 | `(test #t (nan? +nan.0i))` |
| 803 | `(test #t (nan? +nan.0+nan.0i))` |
| 804 | `(test #t (nan? +nan.0+inf.0i))` |
| 805 | `(test #t (nan? +inf.0+nan.0i))` |
| 806 | `(test #f (nan? 1+2i))` |
| 1028 | `(test 0.0+1.0i (inexact (sqrt -1)))` |
| 1029 | `(test 0.0+1.0i (sqrt -1.0-0.0i))` |

### (scheme complex), omitted library (6)

These would have formed a `complex.scm`. `(scheme complex)` is "Omitted, no
complex numbers" (§3.1 table):

| line | test |
|-----:|------|
| 1042 | `(test 1+2i (make-rectangular 1 2))` |
| 1044 | `(test 0.54030230586814+0.841470984807897i (make-polar 1 1))` |
| 1046 | `(test 1 (real-part 1+2i))` |
| 1048 | `(test 2 (imag-part 1+2i))` |
| 1050 | `(test 2.23606797749979 (magnitude 1+2i))` |
| 1052 | `(test 1.10714871779409 (angle 1+2i))` |

### eval.scm (1)

| line | test | reason |
|-----:|------|--------|
| 1958 | `(test 20 (let ((f (eval '(lambda (f x) (f x x)) (null-environment 5)))) (f + 10)))` | `null-environment` exists only in `(scheme r5rs)`, which is "Omitted" (§3.1 table). |

### read.scm, "Numeric syntax" (34)

No exact rationals (§3.2): these read a non-integral exact ratio.

| line | test |
|-----:|------|
| 2376 | `(test-numeric-syntax "1/2" (/ 1 2))` |
| 2377 | `(test-numeric-syntax "#e1/2" (/ 1 2) "1/2")` |
| 2379 | `(test-numeric-syntax "-1/2" (- (/ 1 2)))` |
| 2448 | `(test-numeric-syntax "#x11/2" (/ 17 2) "17/2")` |
| 2449 | `(test-numeric-syntax "#d11/2" (/ 11 2) "11/2")` |
| 2450 | `(test-numeric-syntax "#o11/2" (/ 9 2) "9/2")` |
| 2451 | `(test-numeric-syntax "#b11/10" (/ 3 2) "3/2")` |

These are kept, because their value is an integer or is inexact, so they
need no rational type: `"10/2"`, `"0/10"`, `"#e0/10"`, `"#i3/2"`,
`"#i#x1/10"`, `"#x#i1/10"`, `"#x10/2"`.

No complex numbers (§3, `(scheme complex)` omitted). Comment-only lines in
this block (lines 2383, 2397, 2404, 2406, 2409, 2410, 2415–2417 and
2452–2459, some of which hold commented-out upstream tests) were removed
with it:

| line | test |
|-----:|------|
| 2384 | `(test-numeric-syntax "1+2i" (make-rectangular 1 2))` |
| 2385 | `(test-numeric-syntax "1+2I" (make-rectangular 1 2) "1+2i")` |
| 2386 | `(test-numeric-syntax "1-2i" (make-rectangular 1 -2))` |
| 2387 | `(test-numeric-syntax "-1+2i" (make-rectangular -1 2))` |
| 2388 | `(test-numeric-syntax "-1-2i" (make-rectangular -1 -2))` |
| 2389 | `(test-numeric-syntax "+i" (make-rectangular 0 1) "+i" "+1i" "0+i" "0+1i")` |
| 2390 | `(test-numeric-syntax "0+i" (make-rectangular 0 1) "+i" "+1i" "0+i" "0+1i")` |
| 2391 | `(test-numeric-syntax "0+1i" (make-rectangular 0 1) "+i" "+1i" "0+i" "0+1i")` |
| 2392 | `(test-numeric-syntax "-i" (make-rectangular 0 -1) "-i" "-1i" "0-i" "0-1i")` |
| 2393 | `(test-numeric-syntax "0-i" (make-rectangular 0 -1) "-i" "-1i" "0-i" "0-1i")` |
| 2394 | `(test-numeric-syntax "0-1i" (make-rectangular 0 -1) "-i" "-1i" "0-i" "0-1i")` |
| 2395 | `(test-numeric-syntax "+2i" (make-rectangular 0 2) "2i" "+2i" "0+2i")` |
| 2396 | `(test-numeric-syntax "-2i" (make-rectangular 0 -2) "-2i" "0-2i")` |
| 2398 | `(test-numeric-syntax "1.0+2i" (make-rectangular 1.0 2) "1.0+2.0i" "1.0+2i" "1.+2i" "1.+2.i")` |
| 2399 | `(test-numeric-syntax "1+2.0i" (make-rectangular 1 2.0) "1.0+2.0i" "1+2.0i" "1.+2.i" "1+2.i")` |
| 2400 | `(test-numeric-syntax "1e2+1.0i" (make-rectangular 100.0 1.0) "100.0+1.0i" "100.+1.i")` |
| 2401 | `(test-numeric-syntax "1s2+1.0i" (make-rectangular 100.0 1.0) "100.0+1.0i" "100.+1.i")` |
| 2402 | `(test-numeric-syntax "1.0+1e2i" (make-rectangular 1.0 100.0) "1.0+100.0i" "1.+100.i")` |
| 2403 | `(test-numeric-syntax "1.0+1s2i" (make-rectangular 1.0 100.0) "1.0+100.0i" "1.+100.i")` |
| 2405 | `(test-numeric-syntax "1/2+3/4i" (make-rectangular (/ 1 2) (/ 3 4)))` |
| 2407 | `(test-numeric-syntax "0.5+3/4i" (make-rectangular 0.5 (/ 3 4)) "0.5+0.75i" ".5+.75i" "0.5+3/4i" ".5+3/4i" "500.0e-3+750.0e-3i")` |
| 2411 | `(test-numeric-syntax "+inf.0+inf.0i" (make-rectangular +inf.0 +inf.0) "+Inf.0+Inf.0i")` |
| 2412 | `(test-numeric-syntax "-inf.0+inf.0i" (make-rectangular -inf.0 +inf.0) "-Inf.0+Inf.0i")` |
| 2413 | `(test-numeric-syntax "-inf.0-inf.0i" (make-rectangular -inf.0 -inf.0) "-Inf.0-Inf.0i")` |
| 2414 | `(test-numeric-syntax "+inf.0-inf.0i" (make-rectangular +inf.0 -inf.0) "+Inf.0-Inf.0i")` |
| 2454 | `(test-numeric-syntax "#d1.0+1.0i" (make-rectangular 1.0 1.0) "1.0+1.0i" "1.+1.i")` |
| 2455 | `(test-numeric-syntax "#d10+11i" (make-rectangular 10 11) "10+11i")` |

### Not excluded: literal mutation, Unicode

- **Literal constants are read-only (§3.2).** No retained upstream test
  mutates a quoted list, vector, string or bytevector. Every mutation target
  is built with `list`, `vector`, `string`, `make-string`, `string-copy`,
  `make-vector`, `make-list` or `bytevector`, so nothing needed rewriting.
  Literals are used only as sources, for example
  `(vector-copy! vec 1 #(a b c d e) 0 2)`.
- **Unicode.** Tests that need non-ASCII case mapping or classification are
  not excluded. They were moved to `char-unicode.scm`, to be run with
  `--unicode=full` (§3.1 table, §11.1 flags). Tests that only *contain* non-ASCII
  characters (`#\x1F700`, `#\x10F700`, `"λ"`, `#\λ` read from a string) stay
  in `base.scm` and `read.scm`.

## Expected failures on Chibi

Chibi Scheme was the reference used to check the test files and the
`(chibi test)` shim. Chibi has the full numeric tower, so it gives the
upstream answer for every rewritten test. The shim requires exactness to
match, so all 7 rewritten tests fail on Chibi, as intended. No other test
fails on Chibi master.

| file | test | Chibi result |
|------|------|--------------|
| `base.scm` | `(test 0.15 (/ 3 4 5))` | exact `3/20` |
| `base.scm` | `(test 0.3333333333333333 (/ 3))` | exact `1/3` |
| `base.scm` | `(test-error (/ -4611686018427387904 -1))` | bignum `4611686018427387904`, nothing raised |
| `base.scm` | `(test-error (quotient -4611686018427387904 -1))` | bignum `4611686018427387904`, nothing raised |
| `base.scm` | `(test 3.0 (numerator (/ 6 4)))` | exact `3` |
| `base.scm` | `(test 2.0 (denominator (/ 6 4)))` | exact `2` |
| `inexact.scm` | `(test 3.6217303822937628 c)` | exact `1800/497` |

So on Chibi the expected final lines are
`658 tests, 652 passed, 6 failed` for `base.scm` and
`41 tests, 40 passed, 1 failed` for `inexact.scm`, with exit status 1. Every
other file reports 0 failed.

Chibi 0.12 (the Homebrew release) is older than the upstream test file and
has two bugs that master fixes. It loops forever on
`(string->number "#x4000000000000001/3e1")` in `base.scm`, and it fails the
`(syntax-rules (b) ((_ b ...) #t) ...)` literal-versus-ellipsis test in
4.3. Both also fail when the unmodified upstream file is run with Chibi's
own harness. Validate with Chibi built from master.
