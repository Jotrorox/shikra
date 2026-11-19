;; The extension libraries with --unsafe (ext_unsafe.flags): the checks are gone, assume is trusted;
;; results on valid arguments are unchanged (the interpreter is always checked).
(import (scheme base) (scheme write) (shikra fixnum) (shikra flonum) (shikra f64vector) (shikra assume))
(define (sum-sq n)
  (let loop ((i 0) (acc 0))
    (if (fx<? i n) (loop (fx+ i 1) (fx+ acc (fx* i i))) acc)))
(define (norm v)
  (let loop ((i 0) (s 0.0))
    (if (fx<? i (f64vector-length v)) (loop (fx+ i 1) (fl+* (f64vector-ref v i) (f64vector-ref v i) s)) (flsqrt s))))
(define (first-of p) (assume (pair? p)) (car p))
(define (inc x) (assume (fixnum? x)) (fx+ x 1))
(write (list (sum-sq 1000) (norm (f64vector 3.0 4.0)) (first-of '(a b)) (inc 41) (fxarithmetic-shift 1 61)
             (flgamma 6.0) (flhypot 5.0 12.0) (fxbit-count 255)))
(newline)
