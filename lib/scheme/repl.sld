;; (scheme repl) -- REPL only (spec §3.1): the environment of the REPL session (in a program
;; run by the interpreter: an open top level importing the REPL's default libraries).
(define-library (scheme repl)
  (import (shikra primitives))
  (export interaction-environment)
  (begin
    (define (interaction-environment) (%interaction-environment))))
