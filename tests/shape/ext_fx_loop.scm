;; (shikra fixnum), spec §10: fx operations on a value of unknown type compile to the same code as
;; the generic operations on proven fixnums, plus one type check where the unknown value enters.
(import (scheme base) (scheme write) (shikra fixnum))
(define (hide x) (vector-ref (vector x) 0)) ; a value the analysis cannot see through
(define (sum-fx n) ; n: unknown type
  (let loop ((i 0) (acc 0))
    (if (fx<? i n) (loop (fx+ i 1) (fx+ acc i)) acc)))
(define (sum-generic n) ; n: proven fixnum (its only call passes one)
  (let loop ((i 0) (acc 0))
    (if (< i n) (loop (+ i 1) (+ acc i)) acc)))
(display (list (sum-fx (hide 100000)) (sum-generic 100000)))
(newline)
