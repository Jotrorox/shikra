;; Proper tail calls in constant stack space (10^7 iterations each).
(import (scheme base) (scheme write))

(define n 10000000)

;; mutual recursion between known top-level procedures
(define (my-even? k) (if (= k 0) #t (my-odd? (- k 1))))
(define (my-odd? k) (if (= k 0) #f (my-even? (- k 1))))
(write (my-even? n))
(newline)

;; tail call through an unknown procedure (passed as an argument)
(define (count-down self k) (if (= k 0) 'done (self self (- k 1))))
(write (count-down count-down n))
(newline)

;; tail call through closures with free variables, stored in a vector
(define table (vector #f #f))
(define (make-ping limit) (lambda (k) (if (= k limit) 'ping ((vector-ref table 1) (+ k 1)))))
(define (make-pong limit) (lambda (k) (if (= k limit) 'pong ((vector-ref table 0) (+ k 1)))))
(vector-set! table 0 (make-ping n))
(vector-set! table 1 (make-pong n))
(write ((vector-ref table 0) 0))
(newline)

;; local mutual recursion (letrec) and a named let
(define (local-parity k)
  (letrec ((ev? (lambda (k) (if (= k 0) 'even (od? (- k 1)))))
           (od? (lambda (k) (if (= k 0) 'odd (ev? (- k 1))))))
    (ev? k)))
(write (local-parity (+ n 1)))
(newline)
(write (let loop ((i 0) (acc 0)) (if (= i n) acc (loop (+ i 1) (+ acc 1)))))
(newline)

;; tail calls through apply and through a case-lambda dispatcher
(define (via-apply k) (if (= k 0) 'applied (apply via-apply (list (- k 1)))))
(write (via-apply 1000000))
(newline)
(define (cps k done) (if (= k 0) (done 'cps) (cps (- k 1) (lambda (v) (done v)))))
(write (cps 1000000 (lambda (v) v)))
(newline)
