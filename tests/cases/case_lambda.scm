;; case-lambda: clause selection at known calls (compile time) and unknown calls (run time).
(import (scheme base) (scheme write) (scheme case-lambda))
(define (show x) (write x) (newline))

(define area
  (case-lambda
    ((r) (* 3 r r))
    ((w h) (* w h))
    ((a b c . more) (list 'many a b c more))))
(show (list (area 2) (area 3 4) (area 1 2 3) (area 1 2 3 4 5)))

;; unknown calls go through the dispatcher
(define procs (list area))
(show (map (lambda (f) (list (f 2) (f 3 4))) procs))
(show (apply area '(1 2 3 4)))

;; clauses that call each other, and a local case-lambda capturing a variable
(define range
  (case-lambda
    ((n) (range 0 n))
    ((a b) (if (>= a b) '() (cons a (range (+ a 1) b))))))
(show (list (range 4) (range 2 5)))
(define (make-adder base)
  (case-lambda
    (() base)
    ((x) (+ base x))
    ((x . ys) (apply + base x ys))))
(define add10 (make-adder 10))
(show (list (add10) (add10 1) (add10 1 2 3)))

;; optional arguments in the standard library are case-lambdas too
(show (list (number->string 255 2) (string->list "hello" 2) (vector->list #(1 2 3 4) 1 3)))
