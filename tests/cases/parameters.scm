;; Parameter objects and parameterize (R7RS 4.2.6; spec §8.4): bindings live in the winders
;; list, so escapes restore them.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))
;; R7RS 4.2.6
(define radix
  (make-parameter 10 (lambda (x)
                       (if (and (exact-integer? x) (<= 2 x 16)) x (error "invalid radix")))))
(define (f n) (number->string n (radix)))
(show (f 12))
(show (parameterize ((radix 2)) (f 12)))
(show (f 12))
(show (guard (e ((error-object? e) (error-object-message e))) (parameterize ((radix 0)) (f 12))))
;; parameters are procedures; converters run on creation and parameterize, not on restore
(define p (make-parameter 1))
(define q (make-parameter 10 (lambda (x) (* x 2))))
(show (list (procedure? p) (p) (q)))
(show (parameterize ((q 5)) (q)))
(show (q))
;; nested, several at once, shadowing
(show (parameterize ((p 2) (q 1))
        (list (p) (q) (parameterize ((p 3)) (list (p) (q))) (p))))
(show (list (p) (q)))
(show (parameterize () 'empty))
;; values are converted before any binding takes effect
(show (parameterize ((p 100) (q (p))) (q)))
;; escapes restore the bindings: call/cc, raise caught outside, guard body
(show (list (call/cc (lambda (k) (parameterize ((p 'inner)) (k (p))))) (p)))
(show (guard (e (#t (list e (p)))) (parameterize ((p 'raised)) (raise (p)))))
(show (parameterize ((p 'outer))
        (list (guard (e (#t (p))) (parameterize ((p 'deeper)) (raise 'x))) (p))))
;; a body that returns several values
(show (call-with-values (lambda () (parameterize ((p 2)) (values (p) (p)))) list))
;; current-output-port is a parameter
(define out (open-output-string))
(parameterize ((current-output-port out))
  (display "hidden ")
  (write 'text))
(show (get-output-string out))
(show (eq? (current-output-port) (current-output-port)))
;; setting a parameter (unspecified in R7RS: Shikra sets the innermost binding, converted)
(parameterize ((q 1)) (q 4) (show (q)))
(show (q))
