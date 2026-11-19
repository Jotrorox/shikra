;; (shikra assume) -- SRFI 145 (spec §10): (assume expression message ...) returns the value of
;; expression; a false value raises "assume: assumption violated" with the messages as irritants.
;; The form is compiler-known (expand.c): the analysis uses the assumption as it uses the test of
;; an if, in safe mode too; with --unsafe the check is removed and the fact is trusted. The
;; interpreter always checks.
(define-library (shikra assume)
  (import (shikra primitives))
  (export assume))
