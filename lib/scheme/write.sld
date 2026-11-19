;; (scheme write) -- R7RS 6.13.3. TODO(M5): datum labels for cycles (write, write-shared).
(define-library (scheme write)
  (import (shikra primitives) (only (scheme base) current-output-port))
  (export display write write-simple)
  (begin
    (define display
      (case-lambda ((x) (%display x (current-output-port))) ((x p) (%display x p))))
    (define write
      (case-lambda ((x) (%write x (current-output-port))) ((x p) (%write x p))))
    (define write-simple
      (case-lambda ((x) (%write x (current-output-port))) ((x p) (%write x p))))))
