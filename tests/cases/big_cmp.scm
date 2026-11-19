;; --bignum: comparisons and integer division of a proven "fixnum or bignum" with fixnum constants (the
;; inline forms of emit.c: word comparison when both are fixnums, generic otherwise). Expected
;; output: tests/cases/inc/bignum_ref.py.
(import (scheme base) (scheme write))
(define (f x) ; x: fixnum or bignum (from overflow-capable arithmetic)
  (let ((y (if (exact-integer? x) (* x x) 0)))
    (list (= y 4) (< y 4) (> y 4) (<= y 4) (>= y 4) (= 4 y) (< 4 y) (> 4 y) (<= 4 y) (>= 4 y)
          (quotient y 3) (remainder y 3) (modulo y -3) (quotient y -7) (modulo y 7))))
(for-each (lambda (x) (write (f x)) (newline)) (list 2 -2 3 0 3037000500 -3037000500 (expt 2 40) 2147483648))
