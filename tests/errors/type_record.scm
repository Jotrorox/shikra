;; a provably wrong type through local flow: the accessor applied to a fixnum computed here
(import (scheme base))
(define-record-type point (make-point x y) point? (x point-x) (y point-y))
(define (g n) (let ((k (+ n 1))) (point-x (* k 2))))
(g 1)
