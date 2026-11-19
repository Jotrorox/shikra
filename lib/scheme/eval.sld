;; (scheme eval) -- REPL only (spec §3.1). The interpreter implements %eval and %environment
;; (DESIGN.md §11 M3); importing this library into a compiled program is a compile error.
;; An environment is an open top level (like the REPL's) with the given imports.
(define-library (scheme eval)
  (import (shikra primitives))
  (export eval environment)
  (begin
    (define (eval expr env) (%eval expr env))
    (define (environment . import-sets) (%environment import-sets))))
