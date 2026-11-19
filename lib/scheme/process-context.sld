;; (scheme process-context) -- R7RS 6.14. command-line is the program's argv: a compiled
;; program's own, or, under `shikra run FILE args` and `shikra interp FILE args`, FILE then the
;; arguments. exit unwinds (runs outstanding dynamic-wind after thunks) before it flushes the
;; ports and exits; emergency-exit does not unwind. Exit status: #t or no argument 0, #f 1, an
;; exact integer itself, anything else 0.
(define-library (scheme process-context)
  (import (shikra primitives) (shikra dynamic))
  (export command-line exit emergency-exit get-environment-variable get-environment-variables)
  (begin
    (define (command-line) (%command-line))
    (define (get-environment-variable name) (%get-environment-variable name))
    (define (get-environment-variables) (%get-environment-variables))
    (define exit
      (case-lambda
        (() (travel-to! '()) (%exit #t))
        ((obj) (travel-to! '()) (%exit obj))))
    (define emergency-exit (case-lambda (() (%exit #t)) ((obj) (%exit obj))))))
