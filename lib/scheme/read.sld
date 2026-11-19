;; (scheme read) -- R7RS 6.13.2: the shared runtime reader (rt_read.c), linked only into
;; programs that call read (spec §3.1, §9.1), together with the symbol table. Reader errors
;; satisfy read-error?.
(define-library (scheme read)
  (import (shikra primitives) (only (scheme base) current-input-port))
  (export read)
  (begin
    (define read
      (case-lambda (() (%read (current-input-port))) ((p) (%read p))))))
