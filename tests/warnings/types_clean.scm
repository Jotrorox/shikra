;; Valid programs that must not be diagnosed (spec §11.3): type-dispatching procedures called
;; with one type only (the other branches are dead), guarded accesses, a checked primitive on a
;; path the analysis cannot decide, values that come back through a data structure.
(import (scheme base) (scheme write))
(define (describe x) (cond ((pair? x) (car x)) ((string? x) (string-length x)) ((vector? x) (vector-ref x 0)) (else x)))
(define (safe-car x) (if (pair? x) (car x) 'none))
(define (len v) (if (vector? v) (vector-length v) (string-length v)))
(define (pick flag) (if flag "str" 7))
(define box (list 5))
(display (list (describe 5) (describe 6) (safe-car 'a) (safe-car 3) (len "abcd")))
(display (let ((v (pick (> (length box) 0)))) (if (string? v) (string-length v) (+ v 1))))
(display (if (pair? (car box)) (car (car box)) 'atom))
(newline)
