;; (scheme lazy) for Chez Scheme 10 -- see base.sls.
;; delay-force is (delay (force e)): correct, but not iterative in constant
;; space as R7RS requires.  promise? is not provided (Chez promises are
;; opaque procedures).  No benchmark imports (scheme lazy).
(library (scheme lazy)
  (export delay delay-force force make-promise)
  (import (chezscheme))
  (define-syntax delay-force
    (syntax-rules () ((_ e) (delay (force e)))))
  (define (make-promise v) (delay v)))
