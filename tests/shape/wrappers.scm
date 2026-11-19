;; The library's wrapper layer dissolves: cxr chains, variadic arithmetic and comparisons,
;; optional arguments of known calls.
(import (scheme base) (scheme cxr) (scheme write))
(define (third l) (caddr l))
(define (add3 a b c) (+ a b c))
(define (between? a b c) (< a b c))
(define (fill v) (vector-fill! v 7) v)
(display (list (third '(1 2 3)) (add3 1 2 3) (between? 1 2 3) (fill (make-vector 2))))
(newline)
