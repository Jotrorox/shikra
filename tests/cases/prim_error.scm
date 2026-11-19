;; Primitives are checked: a type error names the procedure.
(import (scheme base) (scheme write))
(define (first x) (car x))
(write (first '(1 2)))
(newline)
(write (first 5))
