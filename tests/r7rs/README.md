# R7RS conformance suite

This is Chibi Scheme's `r7rs-tests.scm` (unmodified copy and license in
`upstream/`), split into one R7RS program per library and adapted to
Shikra's documented implementation restrictions. `EXCLUSIONS.md` lists every
removed or rewritten test and the reason for each.

```
tests/r7rs/
  chibi/test.sld        portable (chibi test): syntax-rules + (scheme base/write/process-context)
  base.scm              (scheme base)                       658 tests
  char.scm              (scheme char), ASCII                 68
  char-unicode.scm      (scheme char), --unicode=full only   46
  inexact.scm           (scheme inexact)                     41
  lazy.scm              (scheme lazy)                        12
  case-lambda.scm       (scheme case-lambda)                 13
  file.scm              (scheme file)                         4
  read.scm              (scheme read) (+ write for numeric syntax) 210
  write.scm             (scheme write)                       57
  process-context.scm   (scheme process-context)              4
  time.scm              (scheme time)                         4
  eval.scm              (scheme eval), REPL only              3
  bignum.scm            (scheme base) bignums, --bignum only   7
  EXCLUSIONS.md, upstream/
```

There is no `cxr.scm`, `load.scm` or `repl.scm`: upstream has no tests for
those libraries (see `EXCLUSIONS.md`).

## Running

Each file is a complete program: `(import ...)`, then `(test-begin ...)` …
`(test-end)`. `(chibi test)` is found as `chibi/test.sld` next to the
program (spec §4.3), so run the files in place:

```
shikra run tests/r7rs/base.scm                  # compiled
shikra interp tests/r7rs/base.scm               # interpreted (M3+; or shikra repl FILE)
shikra run --unicode=full tests/r7rs/char-unicode.scm
shikra run --bignum tests/r7rs/bignum.scm      # the upstream bignum tests (EXCLUSIONS.md)
```

The last line of output is always exactly

```
N tests, P passed, F failed
```

and the exit status is non-zero iff F > 0. A run that does not end with that
line also failed: it crashed, or an error was raised outside any test (each
test catches its own exceptions and counts them as failures). Group
summaries (`-- name: P of N passed[, F FAILED]`) and failure details (the
source expression, the expected value and the actual value) come before
that line. For compiled runs, a compile error is a failure too. The tests that
spec §11.3 says Shikra rejects at compile time have been excluded, so any
compile error is a bug.

Compare runs from both engines by their whole stdout. From M3 on, the
compiled and interpreted output must be identical.

## Which files gate which milestone (spec §13)

| milestone | files | engine / flags |
|-----------|-------|----------------|
| M1 Language | `base.scm` | compiled |
| M2 Collector | `base.scm` | compiled, GC stress mode |
| M3 REPL | `base.scm`, `eval.scm` | interpreter, output identical to the compiled run of `base.scm` |
| M5 Complete | every file except `eval.scm` and `char-unicode.scm` | compiled **and** interpreted |
| M5 Complete | `eval.scm` | interpreter only (`(scheme eval)` is REPL only, spec §3.1) |
| M5 Complete | `char-unicode.scm` | both engines, with `--unicode=full` |
| M6 Portable | the M5 set | on macOS arm64, Linux x86-64, Linux arm64 |

Under the default `--unicode=ascii`, `char-unicode.scm` is expected to fail.
Do not run it without `--unicode=full`.

### Known tension with M1

The M1 bullet in spec §13 lists "tier 1 and 2 continuations", and tier 3 is
scheduled for M5. But `base.scm` (6.10 Control Features) keeps upstream's
re-entrant `dynamic-wind` test, which stores a continuation and calls it
after `call/cc` has returned. That is tier 3 (spec §8.2):

```scheme
(test '(connect talk1 disconnect connect talk2 disconnect)
    (let ((path '()) (c #f))
      (let ((add (lambda (s) (set! path (cons s path)))))
        (dynamic-wind
          (lambda () (add 'connect))
          (lambda () (add (call-with-current-continuation
                            (lambda (c0) (set! c c0) 'talk1))))
          (lambda () (add 'disconnect)))
        (if (< (length path) 4) (c 'talk2) (reverse path)))))
```

Until tier 3 exists, M1 can be met by `base.scm` with this one failure
(`658 tests, 657 passed, 1 failed`). The test is part of `(scheme base)`
and was not removed. (Resolved in M5: with tier 3 continuations the compiled
run passes it too, `658 tests, 658 passed, 0 failed`; DESIGN.md "Tier 3
continuations as built".)

## Validation against Chibi Scheme

The test files and the shim were checked against Chibi Scheme built from
the same master commit as `upstream/` (9b0c19a). The Homebrew 0.12 release
predates the tests and hangs on one of them. Run:

```
chibi-scheme -I tests/r7rs tests/r7rs/base.scm
```

`-I` puts `tests/r7rs` first in the search path, so this shim shadows
Chibi's own `(chibi test)`. You can tell which one ran from the output: this
shim prints `N tests, P passed, F failed`, and Chibi's prints `N out of M
(...%) tests passed`. On Chibi every file reports 0 failed, except the 7
rewritten tests whose Shikra answer differs from the full numeric tower. They
are listed under "Expected failures on Chibi" in `EXCLUSIONS.md`
(`base.scm`: 6, `inexact.scm`: 1).

## Comparison semantics

`test` compares with `equal?`, with one difference: an inexact real
expected value matches an inexact real result within a relative error of
1e-5, and NaN matches NaN. The comparison recurses through pairs and
vectors. Exactness must match. This is stricter than upstream `(chibi test)`,
which accepts an exact result for an inexact expectation, and the stricter
rule is what makes the rewritten rational tests check something.
`test-error` passes iff evaluating the expression raises any object.
