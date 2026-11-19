(import (scheme base))
(define-syntax pair-up (syntax-rules () ((_ a b) (cons a b))))
(pair-up 1 2 3)
