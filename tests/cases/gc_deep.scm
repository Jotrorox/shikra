;; Collector: a 10^6-element list, a tree nested 10^6 deep in the car (each level also
;; holds a fresh side list in the cdr, so marking needs a deep mark stack: it overflows
;; the stack of a --gc-stress build and exercises the rescan), cyclic structures, all kept
;; alive across many collections and verified afterwards without deep recursion.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))
(define big (let loop ((i 0) (l '())) (if (= i 1000000) l (loop (+ i 1) (cons i l)))))
(define tree (let loop ((i 0) (t '())) (if (= i 1000000) t (loop (+ i 1) (cons t (list i (* 2 i)))))))
(define ring (list 'a 'b 'c))
(set-cdr! (cddr ring) ring)                    ; circular list
(define v (vector 1 2 #f))
(vector-set! v 2 v)                            ; vector containing itself
(define (churn n) (let loop ((i 0) (acc '())) (if (= i n) (length acc) (loop (+ i 1) (if (= (modulo i 1000) 0) '() (cons i acc))))))
(show (churn 3000000))
(show (let loop ((l big) (s 0) (n 0)) (if (null? l) (list n s) (loop (cdr l) (+ s (car l)) (+ n 1)))))
(show (let loop ((t tree) (d 0) (ok #t))
        (if (null? t) (list 'depth d ok)
            (let ((i (- 999999 d)))
              (loop (car t) (+ d 1) (and ok (equal? (cdr t) (list i (* 2 i)))))))))
(show (list (car ring) (list-ref ring 3) (eq? (list-tail ring 3) ring)))
(show (eq? (vector-ref (vector-ref v 2) 2) v))
