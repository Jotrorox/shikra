;; (shikra f64vector) -- vectors of raw doubles with the SRFI 4 names (spec §6.2, §10). Elements
;; are unboxed in memory and, for the compiler, in f64vector-ref / f64vector-set!. Stores accept
;; any real number (converted to a flonum). Literal syntax #f64(1.0 2.0) reads and prints.
(define-library (shikra f64vector)
  (import (shikra primitives))
  (export make-f64vector f64vector f64vector? f64vector-length f64vector-ref f64vector-set!
          f64vector->list list->f64vector)
  (begin
    (define make-f64vector
      (case-lambda
        ((k) (%make-f64vector k 0.0))
        ((k fill) (%make-f64vector k fill))))
    (define (f64vector . xs) (%list->f64vector xs))
    (define (f64vector? x) (%f64vector? x))
    (define (f64vector-length v) (%f64vector-length v))
    (define (f64vector-ref v k) (%f64vector-ref v k))
    (define (f64vector-set! v k x) (%f64vector-set! v k x))
    (define (f64vector->list v) (%f64vector->list v))
    (define (list->f64vector l) (%list->f64vector l))))
