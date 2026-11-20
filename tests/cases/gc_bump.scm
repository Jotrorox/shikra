;; --gc=bump (spec §7.1): a bump allocator that never frees. Many small objects across several
;; 1 MB chunks, a large object of its own mapping, strings and closures; the results must be the
;; same as with the collector.
(import (scheme base) (scheme write))
(define (make-list* n f) (let loop ((i 0) (acc '())) (if (= i n) acc (loop (+ i 1) (cons (f i) acc)))))
(define pairs (make-list* 200000 (lambda (i) (cons i (* 2 i)))))
(define big (make-vector 100000 1.5))
(define strs (make-list* 1000 (lambda (i) (string-append "s" (number->string i)))))
(define adders (make-list* 100 (lambda (i) (lambda (x) (+ x i)))))
(define (sum l f) (let loop ((l l) (s 0)) (if (null? l) s (loop (cdr l) (+ s (f (car l)))))))
(write (list (sum pairs cdr) (vector-length big) (vector-ref big 99999) (car strs) (length strs)
             (sum adders (lambda (a) (a 1)))))
(newline)
