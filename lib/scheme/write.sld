;; (scheme write) -- R7RS 6.13.3. write and display label cycles only (so they terminate on
;; circular data), write-shared labels all shared pairs and vectors, write-simple none
;; (DESIGN.md "M5 as built", datum labels in rt_io.c).
(define-library (scheme write)
  (import (shikra primitives) (only (scheme base) current-output-port))
  (export display write write-shared write-simple)
  (begin
    (define display
      (case-lambda ((x) (%display x (current-output-port))) ((x p) (%display x p))))
    (define write
      (case-lambda ((x) (%write x (current-output-port))) ((x p) (%write x p))))
    (define write-shared
      (case-lambda ((x) (%write-shared x (current-output-port))) ((x p) (%write-shared x p))))
    (define write-simple
      (case-lambda ((x) (%write-simple x (current-output-port))) ((x p) (%write-simple x p))))))
