;; The calling convention: rest lists, more than four arguments (argument buffer),
;; apply with long lists, and argument counts checked at run time for unknown calls.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))

(define (seven a b c d e f g) (list a b c d e f g))
(define (rest-after-six a b c d e f . more) (list a b c d e f more))
(define (all . xs) xs)
(define (two-and-rest a b . r) (list a b r))
(show (seven 1 2 3 4 5 6 7))
(show (rest-after-six 1 2 3 4 5 6))
(show (rest-after-six 1 2 3 4 5 6 7 8 9))
(show (list (all) (all 1) (all 1 2 3 4 5 6)))
(show (list (two-and-rest 1 2) (two-and-rest 1 2 3 4 5 6 7)))

;; the same procedures called through unknown references
(define table (list seven rest-after-six all two-and-rest))
(show ((car table) 'a 'b 'c 'd 'e 'f 'g))
(show ((cadr table) 1 2 3 4 5 6 7 8))
(show ((car (cddr table)) 1 2 3 4 5 6 7 8 9 10))

;; a closure with many parameters and many free variables
(define (outer p q r s t)
  (lambda (a b c d e f) (list p q r s t a b c d e f)))
(show ((outer 1 2 3 4 5) 6 7 8 9 10 11))

;; apply with long lists (beyond the static argument buffer)
(define (iota n) (let loop ((i (- n 1)) (acc '())) (if (< i 0) acc (loop (- i 1) (cons i acc)))))
(show (apply + (iota 100)))
(show (length (apply all (iota 300))))
(show (apply max 3 1 (list 4 1 5)))
(show (apply seven 1 2 (list 3 4 5 6 7)))

;; a wrong argument count to an unknown procedure is a run-time error
(define f (car table))
(f 1 2)
