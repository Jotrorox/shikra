;; Re-entrant call/cc patterns are tier 3 (spec §8.2: "the program stores k, returns it, or
;; re-enters it later"): every one of these sites calls rt_call_cc3, and only those; the
;; stack-copying runtime is linked. One site per pattern:
(import (scheme base) (scheme write))
;; 1. store k in a global and call it later
(define saved #f)
(define count 0)
(define (store-global) (call/cc (lambda (k) (set! saved k) 0)))
;; 2. return k from the receiver
(define (return-k) (call/cc (lambda (k) k)))
;; 3. a generator: the resume point is stored in a variable of the generator
(define (make-gen l)
  (define return #f)
  (define (gen)
    (call/cc
     (lambda (r)
       (set! return r)
       (for-each (lambda (x)
                   (call/cc
                    (lambda (resume)
                      (set! gen (lambda () (call/cc (lambda (r2) (set! return r2) (resume #f)))))
                      (return x))))
                 l)
       (return 'done))))
  (lambda () (gen)))
;; 4. amb: choice points pushed on a list
(define fail-stack '())
(define (amb a b)
  (let ((k (call/cc (lambda (k) k))))
    (if (procedure? k)
        (begin (set! fail-stack (cons k fail-stack)) a)
        b)))
(define (fail) (let ((k (car fail-stack))) (set! fail-stack (cdr fail-stack)) (k #f)))
;; 5. a continuation captured in a dynamic-wind body, kept in a vector and re-entered
(define v (make-vector 1 #f))
(define trace '())
(define (wind)
  (dynamic-wind
   (lambda () (set! trace (cons 'in trace)))
   (lambda () (call/cc (lambda (k) (vector-set! v 0 k) 'first)))
   (lambda () (set! trace (cons 'out trace)))))

(let ((x (store-global)))
  (set! count (+ count 1))
  (if (< count 3) (saved count) (begin (write (list 'global x)) (newline))))
(let ((k (return-k)))
  (if (procedure? k) (k 'again) (begin (write (list 'returned k)) (newline))))
(let ((g (make-gen '(a b c))))
  (let* ((x (g)) (y (g)) (z (g)) (w (g)))
    (write (list x y z w))
    (newline)))
(let* ((x (amb 1 2)) (y (amb 10 20)))
  (if (not (= (+ x y) 22)) (fail) (begin (write (list 'amb x y)) (newline))))
(let ((r (wind)))
  (if (eq? r 'first) ((vector-ref v 0) 'second) (begin (write (list r (reverse trace))) (newline))))
