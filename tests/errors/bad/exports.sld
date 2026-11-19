(define-library (bad exports)
  (export present absent)
  (import (scheme base))
  (begin (define present 1)))
