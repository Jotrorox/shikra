(import (scheme base))
(define (f x) (assume (pair? x)) (car x))
(f (list 1))
