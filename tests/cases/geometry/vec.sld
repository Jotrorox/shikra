(define-library (geometry vec)
  (export make-vec vec-x vec-y vec+ (rename vec-scale scale) with-double)
  (import (scheme base))
  (include "vec-impl.scm")
  (begin
    (define (double x) (* 2 x))       ; not exported
    (define-syntax with-double        ; refers to the private double
      (syntax-rules () ((_ e) (double e))))))
