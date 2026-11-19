;; (scheme load) -- REPL only (spec §3.1): read the file's forms one at a time and evaluate
;; each in the environment (default: the interaction environment) before reading the next.
(define-library (scheme load)
  (import (shikra primitives))
  (export load)
  (begin
    (define (load-forms port env)
      (let ((x (%read port)))
        (if (%eof-object? x)
            (begin (%close-port port) (if #f #f))
            (begin (%eval x env) (load-forms port env)))))
    (define load
      (case-lambda
        ((file) (load-forms (%open-input-file file) (%interaction-environment)))
        ((file env) (load-forms (%open-input-file file) env))))))
