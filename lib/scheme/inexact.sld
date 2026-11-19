;; (scheme inexact) -- R7RS 6.2.6. The functions go through the platform layer (rt_sys.c; M6
;; supplies them without libm). No complex numbers (spec §3): a real argument outside a
;; function's real domain gives +nan.0, e.g. (sqrt -4), (log -1), (asin 2). sqrt of an exact
;; perfect square is exact, as R7RS expects of (sqrt 16).
(define-library (scheme inexact)
  (import (shikra primitives) (only (scheme base) exact-integer-sqrt call-with-values))
  (export acos asin atan cos exp finite? infinite? log nan? sin sqrt tan)
  (begin
    (define (finite? z) (%finite? z))
    (define (infinite? z) (%infinite? z))
    (define (nan? z) (%nan? z))
    (define (exp z) (%exp z))
    (define log
      (case-lambda
        ((z) (%log z))
        ((z b) (%/ (%log z) (%log b)))))
    (define (sin z) (%sin z))
    (define (cos z) (%cos z))
    (define (tan z) (%tan z))
    (define (asin z) (%asin z))
    (define (acos z) (%acos z))
    (define atan
      (case-lambda
        ((z) (%atan z))
        ((y x) (%atan2 y x))))
    (define (sqrt z)
      (if (if (%exact-integer? z) (%>= z 0) #f)
          (call-with-values (lambda () (exact-integer-sqrt z))
            (lambda (s r) (if (%= r 0) s (%sqrt z))))
          (%sqrt z)))))
