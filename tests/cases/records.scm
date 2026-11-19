;; define-record-type (R7RS 5.5), at top level and in a body.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))

(define-record-type <point>
  (make-point x y)
  point?
  (x point-x set-point-x!)
  (y point-y))
(define p (make-point 3 4))
(show (list (point? p) (point? 5) (point? (vector 3 4)) (point-x p) (point-y p)))
(set-point-x! p 30)
(show (point-x p))
(show p)

;; constructor taking a subset of the fields, in another order
(define-record-type node (make-node value) node? (next node-next set-node-next!) (value node-value))
(define n1 (make-node 'a))
(define n2 (make-node 'b))
(set-node-next! n1 n2)
(show (list (node-value n1) (node-value (node-next n1)) (node? n2)))

;; distinct record types are distinct
(define-record-type thing (make-thing x) thing? (x thing-x))
(show (list (thing? p) (point? (make-thing 1))))

;; a record type local to a body, used through closures
(define (make-stack)
  (define-record-type stack (mk items) stack? (items stack-items set-stack-items!))
  (let ((s (mk '())))
    (lambda (op . args)
      (case op
        ((push) (set-stack-items! s (cons (car args) (stack-items s))))
        ((pop) (let ((top (car (stack-items s)))) (set-stack-items! s (cdr (stack-items s))) top))
        ((items) (stack-items s))))))
(define st (make-stack))
(st 'push 1)
(st 'push 2)
(st 'push 3)
(show (list (st 'pop) (st 'items)))
