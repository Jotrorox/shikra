;; (scheme case-lambda) -- the expander lowers case-lambda to the core form %case-lambda;
;; known calls resolve their clause at compile time.
(define-library (scheme case-lambda)
  (import (shikra primitives))
  (export case-lambda))
