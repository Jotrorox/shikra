;; Spec §9.1 "Collector: the program allocates and --gc=bump is not set" (pay_gc_bump.scm is
;; the same program with --gc=bump).
(import (scheme base) (scheme write))
(define (build n acc) (if (= n 0) acc (build (- n 1) (cons (make-vector 3 n) acc))))
(define (total l) (if (null? l) 0 (+ (vector-ref (car l) 0) (total (cdr l)))))
(display (total (build 10000 '())))
(newline)
