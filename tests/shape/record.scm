;; Record accessors are calls of nothing: the record primitive is applied in place.
(import (scheme base) (scheme write))
(define-record-type point (make-point x y) point? (x point-x) (y point-y))
(define (norm1 p) (+ (point-x p) (point-y p)))
(display (norm1 (make-point 3 4)))
(newline)
