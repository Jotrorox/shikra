;; (scheme process-context) -- M0: exit and emergency-exit only.
;; TODO(M1): command-line, get-environment-variable(s); exit runs outstanding
;; dynamic-wind after thunks.
(define-library (scheme process-context)
  (import (shikra primitives))
  (export exit emergency-exit)
  (begin
    (define exit (case-lambda (() (%exit #t)) ((obj) (%exit obj))))
    (define emergency-exit (case-lambda (() (%exit #t)) ((obj) (%exit obj))))))
