;; bench/ext/vhash.scm -- integer kernel, portable R7RS: a polynomial hash (mod 2^30) and an odd
;; count over a vector of integers, repeated. Values taken out of a vector have no type the
;; analysis knows, so every operation keeps its generic type dispatch.
(import (scheme base) (scheme write) (scheme process-context))
(define (arg k d) (let ((a (command-line))) (if (> (length a) k) (string->number (list-ref a k)) d)))
(define n (arg 1 1000000))
(define reps (arg 2 200))
(define v (make-vector n 0))
(let fill ((i 0) (x 12345)) (when (< i n) (vector-set! v i x) (fill (+ i 1) (modulo (+ (* x 1103515245) 12345) 2147483648))))
(define (hash v)
  (let loop ((i 0) (h 0) (odd 0))
    (if (< i (vector-length v))
        (let ((x (vector-ref v i)))
          (loop (+ i 1) (modulo (+ (* h 31) x) 1073741824) (if (odd? x) (+ odd 1) odd)))
        (+ (* h 1000) (modulo odd 1000)))))
(let loop ((r 0) (s 0)) (if (< r reps) (loop (+ r 1) (modulo (+ s (hash v)) 1000000007)) (begin (write s) (newline))))
