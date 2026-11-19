(define-library (wlib util)
  (import (scheme base) (scheme char))
  (export double triple)
  (begin
    (define (double x) (* 2 x))
    (define (triple x) (* 3 x))
    (define (unused-helper) 1)))
