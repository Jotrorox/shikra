;; Every derived expression type the expander lowers (R7RS 4.2), one or more uses each.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))

;; let, let*, letrec, letrec*, named let
(show (let ((x 1) (y 2)) (let ((x y) (y x)) (list x y))))
(show (let* ((x 1) (y (+ x 1)) (z (* y 2))) (list x y z)))
(show (let* () 'empty-let*))
(show (letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 10)))
(show (letrec* ((a 1) (b (+ a 1)) (c (lambda () (+ a b)))) (c)))
(show (let loop ((i 0) (acc '())) (if (= i 5) (reverse acc) (loop (+ i 1) (cons (* i i) acc)))))
(show (let () (define x 5) (define (twice y) (* 2 y)) (twice x)))

;; cond with else and =>, test-only clauses
(define (classify n)
  (cond ((< n 0) 'negative)
        ((assv n '((0 . zero) (1 . one))) => cdr)
        ((> n 100))
        (else 'other)))
(show (map classify '(-5 0 1 50 200)))
(show (cond (#f 1)))

;; case with lists of data, else, and =>
(define (kind x)
  (case x
    ((1 2 3) 'small)
    ((a b c) 'letter)
    ((#\x) 'char)
    ((10) => (lambda (v) (list 'ten v)))
    (else => (lambda (v) (list 'other v)))))
(show (map kind (list 2 'b #\x 10 "s")))
(show (case 5 ((1) 'one)))

;; and, or, when, unless
(show (list (and) (and 1 2) (and 1 #f 3) (or) (or #f 2) (or #f #f)))
(show (let ((t 5)) (or #f t)))
(show (list (when (> 1 0) 'a 'b) (unless (> 1 0) 'c)))
(when #t (display "when-body") (newline))
(unless #f (display "unless-body") (newline))

;; do
(show (do ((vec (make-vector 5)) (i 0 (+ i 1))) ((= i 5) vec) (vector-set! vec i i)))
(show (let ((x '(1 3 5 7 9))) (do ((x x (cdr x)) (sum 0 (+ sum (car x)))) ((null? x) sum))))

;; quasiquote: unquote, splicing, nesting, vectors, dotted tails
(define x 42)
(define lst '(1 2 3))
(show `(a ,x ,@lst b))
(show `(1 . ,x))
(show `#(v ,x ,@lst))
(show `(a `(b ,(c ,x)) d))
(show `(,@lst . tail))
(show `,x)

;; let-values, let*-values, define-values
(show (let-values (((a b) (values 1 2)) ((c . d) (values 3 4 5)) (all (values 6 7))) (list a b c d all)))
(show (let*-values (((a b) (values 1 2)) ((c) (values (+ a b)))) (list a b c)))
(show (let-values () 'no-bindings))
(define-values (q r) (floor/ 17 5))
(define-values (head . tail) (values 1 2 3))
(define-values all-vals (values 'x 'y))
(show (list q r head tail all-vals))
(define (internal-dv)
  (define-values (u v) (values 10 20))
  (+ u v))
(show (internal-dv))

;; begin splicing of definitions at top level and in bodies
(begin (define b1 1) (define b2 2))
(show (+ b1 b2))
(show (let () (begin (define p 3) (define r 4)) (* p r)))
