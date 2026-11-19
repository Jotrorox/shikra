(import (scheme base))
(define-syntax bad (syntax-rules () ((_ x ...) (list x))))
(bad 1 2)
