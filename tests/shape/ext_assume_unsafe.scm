;; (shikra assume), spec §10: the analysis refines after (assume test) as after an if on the test.
(import (scheme base) (scheme write) (shikra assume))
(define (hide x) (vector-ref (vector x) 0))
(define (inc x) (assume (exact-integer? x) "inc: x") (+ x 1))
(define (head p) (assume (pair? p)) (car p))
(define (scale v k) (assume (and (exact-integer? k) (< k 100))) (* (+ k 1) 2))
(display (list (inc (hide 41)) (head (hide '(a b))) (scale (hide 3) 0)))
(newline)
