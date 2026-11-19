;; (scheme write) for Chez Scheme 10 -- see base.sls.
(library (scheme write)
  (export display write write-shared write-simple)
  (import (chezscheme))
  (define write-shared
    (case-lambda
      ((x) (write-shared x (current-output-port)))
      ((x p) (parameterize ((print-graph #t)) (write x p)))))
  (define write-simple
    (case-lambda
      ((x) (write-simple x (current-output-port)))
      ((x p) (parameterize ((print-graph #f)) (write x p))))))
