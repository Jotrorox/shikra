;; Provably wrong types that are warnings, not errors (spec §11.3, DESIGN.md M4-2): the value
;; reaches the primitive through a user procedure's parameter (even when the procedure is
;; inlined), or the primitive runs inside a guard body (test-error style); the code still raises
;; at run time and the program is valid.
(import (scheme base) (scheme write))
(define (first x) (car x))
(define (caught thunk) (guard (e (#t 'caught)) (thunk)))
(display (caught (lambda () (first 5))))
(display (guard (e ((error-object? e) (error-object-message e))) (vector-ref "abc" 0)))
(display (caught (lambda () (string-length 'sym))))
(newline)
