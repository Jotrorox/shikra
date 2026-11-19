;; Internal definitions become letrec*: forward references from procedures, values
;; computed in order, mutual recursion, and early references detected at run time.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))

(define (body-defs n)
  (define a (* n 2))            ; complex, initialised in order
  (define (get-b) b)            ; refers to a later definition
  (define b (+ a 1))
  (define (ev? k) (if (= k 0) #t (od? (- k 1))))
  (define (od? k) (if (= k 0) #f (ev? (- k 1))))
  (list a b (get-b) (ev? n)))
(show (body-defs 5))

;; an expression between definitions is evaluated in order
(define (mixed)
  (define log '())
  (set! log (cons 'first log))
  (define x (begin (set! log (cons 'second log)) 10))
  (list x (reverse log)))
(show (mixed))

;; letrec with lambdas only: one group of mutually recursive closures with free variables
(define (make-walker limit)
  (letrec ((up (lambda (i acc) (if (= i limit) acc (down (+ i 1) (cons i acc)))))
           (down (lambda (i acc) (up i acc))))
    (lambda () (reverse (up 0 '())))))
(show ((make-walker 5)))

;; top-level forward references from procedures to later definitions
(define (use-later) (* later 2))
(define later 21)
(show (use-later))

;; reading a variable before its initialisation is an error, detected at run time
(define (too-early)
  (define x (y))
  (define (y) x)
  x)
(too-early)
