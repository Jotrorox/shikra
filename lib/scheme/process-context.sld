;; (scheme process-context) -- exit unwinds (runs outstanding dynamic-wind after thunks)
;; before it flushes the ports and exits; emergency-exit does not unwind. Exit status: #t or
;; no argument 0, #f 1, an exact integer itself, anything else 0.
;; TODO(next): command-line, get-environment-variable(s).
(define-library (scheme process-context)
  (import (shikra primitives) (shikra dynamic))
  (export exit emergency-exit)
  (begin
    (define exit
      (case-lambda
        (() (travel-to! '()) (%exit #t))
        ((obj) (travel-to! '()) (%exit obj))))
    (define emergency-exit (case-lambda (() (%exit #t)) ((obj) (%exit obj))))))
