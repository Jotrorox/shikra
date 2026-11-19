;; Every definition and import is used: no warnings.
(import (scheme base) (scheme write))
(define (square x) (* x x))
(define-syntax swap! (syntax-rules () ((_ a b) (let ((t a)) (set! a b) (set! b t)))))
(define p 1)
(define q 2)
(swap! p q)
(display (square p))
(newline)
