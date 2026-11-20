;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: the bignum tests, run with --bignum.
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README):
;; exactly the upstream tests that EXCLUSIONS.md lists as excluded from
;; base.scm because their values are bignums (spec 3.2: optional bignums,
;; behind a flag), restored verbatim, in upstream order and with upstream
;; text.  Run with `shikra run --bignum` (or `shikra interp --bignum`).

(import (scheme base)
        (chibi test))

(test-begin "R7RS bignums (--bignum)")

(test-begin "4.2 Derived expression types")

(test '(815238614083298888 443242361398135744)
    (let*-values (((root rem) (exact-integer-sqrt (expt 2 119))))
      (list root rem)))

(test '(1152921504606846976 0)
    (let*-values (((root rem) (exact-integer-sqrt (expt 2 120))))
      (list root rem)))

(test '(1630477228166597776 1772969445592542976)
    (let*-values (((root rem) (exact-integer-sqrt (expt 2 121))))
      (list root rem)))

(test '(31622776601683793319 62545769258890964239)
    (let*-values (((root rem) (exact-integer-sqrt (expt 10 39))))
      (list root rem)))

(let*-values (((root rem) (exact-integer-sqrt (expt 2 140))))
  (test 0 rem)
  (test (expt 2 140) (square root)))

(test-end)

(test-begin "6.2 Numbers")

;; From R7RS 6.2.6 Numerical operations:
;;
;; These predicates are required to be transitive.
;;
;; _Note:_ The traditional implementations of these predicates in
;; Lisp-like languages, which involve converting all arguments to inexact
;; numbers if any argument is inexact, are not transitive.

;; Example from Alan Bawden
(let ((a (- (expt 2 1000) 1))
      (b (inexact (expt 2 1000))) ; assuming > single-float-epsilon
      (c (+ (expt 2 1000) 1)))
  (test #t (if (and (= a b) (= b c))
               (= a c)
               #t)))

(test-end)

(test-end)
