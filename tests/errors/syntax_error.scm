(import (scheme base))
(define-syntax must-be-two
  (syntax-rules () ((_ a b) (list a b)) ((_ . other) (syntax-error "must-be-two takes two arguments:" other))))
(must-be-two 1)
