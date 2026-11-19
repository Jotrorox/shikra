;; Top-level mutual tail recursion entered from several places (spec §8.1): the two procedures
;; become join points of one C function with a selector; their tail calls are jumps.
(import (scheme base) (scheme write))
(define (my-even? n) (if (= n 0) #t (my-odd? (- n 1))))
(define (my-odd? n) (if (= n 0) #f (my-even? (- n 1))))
(display (list (my-even? 1000001) (my-odd? 7) (my-even? 10)))
(newline)
