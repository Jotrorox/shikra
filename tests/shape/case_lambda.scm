;; Spec §3.1 "(scheme case-lambda): resolved at compile time for known calls" and "(scheme cxr):
;; inlined to chained loads": each known call of area goes straight to its clause's body (no
;; dispatch on the argument count, no call), caddr and cadddr are loads.
(import (scheme base) (scheme case-lambda) (scheme cxr) (scheme write))
(define area
  (case-lambda
    ((r) (* 3 r r))
    ((w h) (* w h))
    ((a b c) (+ a b c))))
(define (go l) (list (area (car l)) (area (car l) (cadr l)) (area 1 2 (caddr l)) (cadddr l)))
(write (go (list 1 2 3 4)))
(newline)
