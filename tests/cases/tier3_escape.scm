;; Tier-3 continuations used purely as escapes (they are stored, so the compiler cannot prove
;; they die with their call/cc): calling one while its call/cc is still active behaves like a
;; tier-2 escape -- after thunks run, handlers and parameters are restored, nothing is copied
;; back. 10^5 escapes in a loop also check that captures do not accumulate.
(import (scheme base) (scheme write))
(define last-k #f)
(define p (make-parameter 0))
(define (find-first pred l)
  (call/cc
   (lambda (return)
     (set! last-k return)
     (for-each (lambda (x) (if (pred x) (return x))) l)
     #f)))
(write (list (find-first even? '(1 3 4 5 6)) (find-first even? '(1 3 5))))
(newline)
(define trace '())
(define v
  (call/cc
   (lambda (k)
     (set! last-k k)
     (parameterize ((p 1))
       (dynamic-wind
        (lambda () (set! trace (cons 'in trace)))
        (lambda ()
          (with-exception-handler
           (lambda (e) (k (list 'escaped e (p))))
           (lambda () (raise 'oops))))
        (lambda () (set! trace (cons 'out trace))))))))
(write (list v (p) (reverse trace)))
(newline)
(define (sum-until-negative l)
  (call/cc
   (lambda (k)
     (set! last-k k)
     (let loop ((l l) (s 0))
       (cond ((null? l) s) ((negative? (car l)) (k s)) (else (loop (cdr l) (+ s (car l)))))))))
(define total 0)
(do ((i 0 (+ i 1))) ((= i 100000))
  (set! total (+ total (sum-until-negative '(1 2 3 -1 5)))))
(write total)
(newline)
(write (procedure? last-k))
(newline)
