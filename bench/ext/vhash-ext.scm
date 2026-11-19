;; bench/ext/vhash-ext.scm -- the same kernel with (shikra fixnum): each element is checked once
;; where it enters (fx* of an unknown value), then everything is tagged-word arithmetic; the
;; modulus by 2^30 is a mask. Guarded with cond-expand, so it runs on any R7RS system.
(import (scheme base) (scheme write) (scheme process-context))
(cond-expand
  (shikra (import (shikra fixnum)))
  (else (begin (define fx+ +) (define fx* *) (define fx<? <) (define fxodd? odd?)
               (define (fxand a m) (modulo a (+ m 1))))))
(define (arg k d) (let ((a (command-line))) (if (> (length a) k) (string->number (list-ref a k)) d)))
(define n (arg 1 1000000))
(define reps (arg 2 200))
(define v (make-vector n 0))
(let fill ((i 0) (x 12345)) (when (< i n) (vector-set! v i x) (fill (+ i 1) (modulo (+ (* x 1103515245) 12345) 2147483648))))
(define (hash v)
  (let loop ((i 0) (h 0) (odd 0))
    (if (fx<? i (vector-length v))
        (let ((x (vector-ref v i)))
          (loop (fx+ i 1) (fxand (fx+ (fx* h 31) x) 1073741823) (if (fxodd? x) (fx+ odd 1) odd)))
        (+ (* h 1000) (modulo odd 1000)))))
(let loop ((r 0) (s 0)) (if (< r reps) (loop (+ r 1) (modulo (+ s (hash v)) 1000000007)) (begin (write s) (newline))))
