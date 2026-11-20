;; --bignum: constant expressions the optimizer sees whole (folded at compile time or left to run time,
;; never wrongly): overflowing fixnum constants, -fixnum-least, big literals, eqv?/equal? of equal big
;; literals. Expected output: tests/cases/inc/bignum_ref.py.
(import (scheme base) (scheme write))
(write (list (* 4611686018427387903 4) (+ 4611686018427387903 1) (- -4611686018427387904 1) (- -4611686018427387904)
  (quotient -4611686018427387904 -1) (abs -4611686018427387904) (expt 2 100) (eqv? 100000000000000000000 100000000000000000000)
  (equal? '(100000000000000000000) '(100000000000000000000)) (= 100000000000000000000 (* 10000000000 10000000000))
  (inexact 100000000000000000000) (exact 1e20) (square 4611686018427387903) (- 100000000000000000000 100000000000000000000)))
(newline)
