;; (shikra flonum), spec §10: a loop over fl operations has double locals and allocates nothing,
;; even when its input comes from a value of unknown type (one check where it enters).
(import (scheme base) (scheme write) (shikra flonum) (shikra fixnum))
(define (hide x) (vector-ref (vector x) 0))
(define (integrate n h) ; midpoint rule for x^2 e^-x on [0, n*h]
  (let loop ((i 0) (acc 0.0))
    (if (fx<? i n)
        (let ((x (fl* (fl+ (inexact i) 0.5) h)))
          (loop (fx+ i 1) (fl+* (fl* x x) (flexp (fl- 0.0 x)) acc)))
        (fl* acc h))))
(display (flround (fl* 1e6 (integrate 100000 (hide 0.0001)))))
(newline)
