;; define-c, spec §10: the C function is called directly with unboxed arguments and result, so
;; (fl+ (c-hypot x y) 1.0) stays a double computation.
(import (scheme base) (scheme write) (shikra ffi) (shikra flonum))
(define-c c-hypot "hypot" (double double) double)
(define (f x y) (fl+ (c-hypot x y) 1.0))
(define (g n) (let loop ((i 0) (s 0.0)) (if (< i n) (loop (+ i 1) (fl+ s (f 3.0 4.0))) s)))
(display (g 10))
(newline)
