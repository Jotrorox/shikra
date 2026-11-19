;; Closures, boxes (variables both captured and assigned), shared state.
(import (scheme base) (scheme write))

(define (make-counter)
  (let ((n 0))
    (lambda () (set! n (+ n 1)) n)))
(define c1 (make-counter))
(define c2 (make-counter))
(c1)
(c1)
(write (list (c1) (c2) (c2)))
(newline)

;; two closures sharing one mutable variable
(define (make-account balance)
  (define (deposit! x) (set! balance (+ balance x)) balance)
  (define (withdraw! x) (set! balance (- balance x)) balance)
  (lambda (msg x)
    (cond ((eq? msg 'deposit) (deposit! x))
          ((eq? msg 'withdraw) (withdraw! x))
          (else (error "unknown request" msg)))))
(define acc (make-account 100))
(acc 'deposit 50)
(write (acc 'withdraw 30))
(newline)

;; a parameter that is captured and assigned
(define (accumulator total)
  (lambda (x) (set! total (+ total x)) total))
(define a (accumulator 10))
(a 5)
(write (a 5))
(newline)

;; closures created in a loop capture each iteration's binding
(define procs
  (let loop ((i 0) (acc '()))
    (if (= i 4) (reverse acc) (loop (+ i 1) (cons (lambda () (* i i)) acc)))))
(write (map (lambda (p) (p)) procs))
(newline)

;; more than four captured variables, nested capture
(define (capture a b c d e f)
  (lambda (g)
    (lambda (h) (list a b c d e f g h))))
(write (((capture 1 2 3 4 5 6) 7) 8))
(newline)

;; an assigned local that is not captured stays a plain C local
(define (count-evens l)
  (let ((n 0))
    (for-each (lambda (x) (if (even? x) (set! n (+ n 1)))) l)
    n))
(write (count-evens '(1 2 3 4 6)))
(newline)
(define (local-assign x)
  (let ((y 1))
    (set! y (+ y x))
    (set! y (* y 2))
    y))
(write (local-assign 4))
(newline)
