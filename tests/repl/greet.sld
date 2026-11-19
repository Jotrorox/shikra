(define-library (greet)
  (import (scheme base))
  (export greeting)
  (begin (define (greeting name) (string-append "hello, " name))))
