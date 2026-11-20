;; Spec §7.1 "--gc=bump replaces the collector with a bump allocator that never frees ... removes
;; the collector from the binary": the program of pay_gc.scm, built with --gc=bump.
(import (scheme base) (scheme write))
(define (build n acc) (if (= n 0) acc (build (- n 1) (cons (make-vector 3 n) acc))))
(define (total l) (if (null? l) 0 (+ (vector-ref (car l) 0) (total (cdr l)))))
(display (total (build 10000 '())))
(newline)
