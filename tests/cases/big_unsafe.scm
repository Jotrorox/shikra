;; --bignum --unsafe: the type checks are gone, but overflow still promotes (it is semantics, not a
;; check): the sum of i^3 for i <= 2,000,000 (about 4e24) and a factorial through an unchecked loop.
;; Expected output: tests/cases/inc/bignum_ref.py.
(import (scheme base) (scheme write))
(define (show . xs) (for-each (lambda (x) (write x) (display " ")) xs) (newline))
(define (sum-cubes n)
  (let loop ((i 1) (s 0)) (if (> i n) s (loop (+ i 1) (+ s (* i i i))))))
(define (sum-to n)
  (let loop ((i 0) (acc 0)) (if (> i n) acc (loop (+ i 1) (+ acc i)))))
(show 'sum (+ 4611686018427387903 1) (- -4611686018427387904 1) (* 4611686018427387903 4611686018427387903) (sum-cubes 2000000))
(show 'fact (let loop ((i 1) (acc 1)) (if (> i 30) acc (loop (+ i 1) (* acc i)))) (* (sum-to 1000000) 1099511627776))
