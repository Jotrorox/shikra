;; Multiple values (R7RS 4.2.2, 5.3.3, 6.10; spec §8.5): literal receivers are join points,
;; unknown receivers get a packed values object, a single value is itself.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))
;; R7RS 6.10
(show (call-with-values (lambda () (values 4 5)) (lambda (a b) b)))
(show (call-with-values * -))
;; zero values, many values, rest receivers
(show (call-with-values (lambda () (values)) list))
(show (call-with-values (lambda () (values)) (lambda () 'none)))
(show (call-with-values (lambda () (values 1 2 3 4 5 6)) list))
(show (call-with-values (lambda () (values 1 2 3 4 5 6)) (lambda (a b . r) (list a b r))))
(define (six) (values 1 2 3 4 5 6))
(show (call-with-values six list))                          ; unknown receiver
(show (call-with-values six (lambda (a b . r) (list b r)))) ; literal receiver, spread
(show (call-with-values (lambda () (six)) (lambda (a b c d e f) (+ a b c d e f))))
(show (call-with-values (lambda () 7) (lambda (x) (* x x))))
(define (pick n) (if (> n 0) (values n 'pos) (values (- n) 'neg)))
(show (call-with-values (lambda () (pick -3)) cons))
;; R7RS 4.2.2
(show (let-values (((root rem) (exact-integer-sqrt 32))) (* root rem)))
(show (let ((a 'a) (b 'b) (x 'x) (y 'y))
        (let*-values (((a b) (values x y)) ((x y) (values a b)))
          (list a b x y))))
(show (let-values (((a b) (values 1 2)) ((c . d) (values 3 4 5)) (all (values 6 7))) (list a b c d all)))
;; R7RS 5.3.3
(define-values (x y) (exact-integer-sqrt 17))
(show (list x y))
(show (let () (define-values (x y) (values 1 2)) (+ x y)))
(define-values (h . t) (values 1 2 3))
(show (list h t))
(define-values all-of-them (values 'p 'q))
(show all-of-them)
;; captured and assigned receiver variables live in boxes
(show (let-values (((a b) (values 1 2)))
        (let ((get (lambda () (list a b)))) (set! a 10) (get))))
;; a receiver that gets the wrong number of values raises a catchable error
(define (three) (values 1 2 3))
(show (guard (e ((error-object? e) (list (error-object-message e) (error-object-irritants e))))
        (let-values (((a b) (three))) a)))
;; values through apply and as first-class
(show (call-with-values (lambda () (apply values '(1 2 3))) list))
(show (call-with-values (lambda () (apply values '(1))) list))
(show (map (lambda (v) (call-with-values (lambda () v) list)) (list 1 2)))
