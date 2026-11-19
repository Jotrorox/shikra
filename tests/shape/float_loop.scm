;; Flonum unboxing (spec §6.2): a loop over doubles inside a procedure with a private signature
;; (double in, double out); a known recursive procedure on doubles.
(import (scheme base) (scheme write))
(define (sum-squares n) ; recursive, so it stays a procedure of its own
  (if (< n 0.0)
      (- (sum-squares (- n)))
      (let loop ((x 0.0) (acc 0.0))
        (if (> x n) acc (loop (+ x 1.0) (+ acc (* x x)))))))
(define (fib n) (if (< n 2.) n (+ (fib (- n 1.)) (fib (- n 2.)))))
(display (sum-squares 1000.0))
(newline)
(display (list (sum-squares -10.0) (fib 20.)))
(newline)
