;; (shikra f64vector), spec §6.2: a dot product over two f64vectors allocates nothing and has no
;; boxed flonum: the elements are loaded as doubles, the sum is a double.
(import (scheme base) (scheme write) (shikra f64vector) (shikra flonum) (shikra fixnum))
(define (dot a b)
  (let loop ((i 0) (s 0.0))
    (if (fx<? i (f64vector-length a))
        (loop (fx+ i 1) (fl+ s (fl* (f64vector-ref a i) (f64vector-ref b i))))
        s)))
(define (fill! v f) (do ((i 0 (fx+ i 1))) ((fx=? i (f64vector-length v)) v) (f64vector-set! v i (f i))))
(define a (fill! (make-f64vector 1000) (lambda (i) (inexact i))))
(define b (fill! (make-f64vector 1000) (lambda (i) 0.5)))
(display (dot a b))
(newline)
