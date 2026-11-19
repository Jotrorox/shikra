;; (scheme process-context) for Chez Scheme 10 -- see base.sls.
(library (scheme process-context)
  (export command-line emergency-exit exit get-environment-variable
          get-environment-variables)
  (import (chezscheme))
  (define emergency-exit
    (case-lambda (() (exit)) ((code) (exit code))))
  (define (get-environment-variable name) (getenv name))
  ;; Not available portably in Chez; no benchmark uses it.
  (define (get-environment-variables) '()))
