;; (shikra assume), SRFI 145: the value of the expression, or an error "assume: assumption
;; violated" with the messages as irritants; the analysis uses the assumption (ext_assume shape test).
(import (scheme base) (scheme write) (shikra assume) (util check))
(check (assume 5) 5)
(check (assume (+ 1 2) "never shown") 3)
(check (assume '()) '())
(define (f x) (assume (exact-integer? x) "f wants an exact integer" x) (+ x 1))
(check (f 41) 42)
(check (guard (e ((error-object? e) (list (error-object-message e) (error-object-irritants e)))) (f 'a))
       '("assume: assumption violated" ("f wants an exact integer" a)))
(check-error "assume" (assume #f))
(check-error "assume" (assume (= 1 2) "one is not two"))
(define (g p) (assume (pair? p)) (car p))
(check (g '(1 2)) 1)
(check-error "assume" (g 5))
(define count 0)
(check (assume (begin (set! count (+ count 1)) #t)) #t) ; evaluated once
(check count 1)
(check-done)
