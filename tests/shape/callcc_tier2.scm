;; Escape-only call/cc patterns stay in tiers 1 and 2 after the tier-3 escape analysis (spec
;; §8.2: "because the standard library is analysed with the program, common patterns stay in
;; tier 1 or 2"): no stack copying is linked. Each site passes k down to known procedures
;; (inlined or analysed library procedures, user recursion) that only call it.
(import (scheme base) (scheme write))
;; early exit from for-each, map, vector-for-each, nested loops
(define (first-negative l) (call/cc (lambda (return) (for-each (lambda (x) (if (negative? x) (return x))) l) #f)))
(define (map-until-zero l) (call/cc (lambda (k) (map (lambda (x) (if (= x 0) (k 'zero) (* 2 x))) l))))
(define (vector-find v x) (call/cc (lambda (k) (vector-for-each (lambda (y) (if (equal? x y) (k #t))) v) #f)))
(define (find-pair m target)
  (call/cc
   (lambda (found)
     (do ((i 0 (+ i 1))) ((= i m) #f)
       (do ((j 0 (+ j 1))) ((= j m))
         (if (= (* i j) target) (found (list i j))))))))
;; recursive search: k passed down a user recursion
(define (tree-find tree pred)
  (call/cc
   (lambda (k)
     (let walk ((t tree))
       (cond ((pair? t) (walk (car t)) (walk (cdr t)))
             ((null? t) #f)
             ((pred t) (k t))
             (else #f)))
     #f)))
(define (search-list l x k) (cond ((null? l) #f) ((eqv? (car l) x) (k 'found)) (else (search-list (cdr l) x k))))
(define (member? l x) (call/cc (lambda (k) (search-list l x k))))
;; error exits without guard: a handler that calls k, an exit continuation passed to a helper
(define (safe-div a b)
  (call/cc
   (lambda (k)
     (with-exception-handler
      (lambda (e) (k 'div-error))
      (lambda () (if (= b 0) (raise 'div0) (quotient a b)))))))
(define (check-all l fail) (for-each (lambda (x) (if (not (number? x)) (fail x))) l) 'ok)
(define (validate l) (call/cc (lambda (fail) (check-all l (lambda (x) (fail (list 'bad x)))))))
;; inside dynamic-wind and parameterize
(define p (make-parameter 1))
(define (wind-exit)
  (call/cc (lambda (k) (dynamic-wind (lambda () #f) (lambda () (parameterize ((p 2)) (k (p)))) (lambda () #f)))))
;; the ctak and fibc benchmark patterns (r7rs-benchmarks)
(define (ctak x y z) (call/cc (lambda (k) (ctak-aux k x y z))))
(define (ctak-aux k x y z)
  (if (not (< y x))
      (k z)
      (call/cc
       (lambda (k)
         (ctak-aux k
                   (call/cc (lambda (k) (ctak-aux k (- x 1) y z)))
                   (call/cc (lambda (k) (ctak-aux k (- y 1) z x)))
                   (call/cc (lambda (k) (ctak-aux k (- z 1) x y))))))))
(define (addc x y k) (if (zero? y) (k x) (addc (+ x 1) (- y 1) k)))
(define (fibc x c)
  (if (zero? x)
      (c 0)
      (if (zero? (- x 1))
          (c 1)
          (addc (call/cc (lambda (c) (fibc (- x 1) c)))
                (call/cc (lambda (c) (fibc (- x 2) c)))
                c))))
;; a guard (its own call/cc is tier 2) inside a dynamic-wind
(define (guarded l) (dynamic-wind (lambda () #f) (lambda () (guard (e (#t 'caught)) (car l))) (lambda () #f)))
(define (show x) (write x) (newline))
(show (list (first-negative '(1 -2 3)) (map-until-zero '(1 0 2)) (vector-find #(1 2 3) 2) (find-pair 10 12)))
(show (list (tree-find '((1 2) (3 (4))) (lambda (x) (> x 2))) (member? '(1 2 3) 2) (member? '(1) 5)))
(show (list (safe-div 10 2) (safe-div 1 0) (validate '(1 2)) (validate '(1 a)) (wind-exit) (p)))
(show (list (ctak 18 12 6) (fibc 20 (lambda (x) x)) (guarded '(1)) (guarded 5)))
