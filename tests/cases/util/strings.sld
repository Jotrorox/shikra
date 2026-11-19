(define-library (util strings)
  (import (scheme base))
  (include-library-declarations "strings-decls.scm")
  (cond-expand
    (shikra (begin (define impl 'shikra)))
    (else (begin (define impl 'other))))
  (cond-expand
    ((and r7rs (not ratios) (library (scheme base))) (export features-ok) (begin (define features-ok #t)))
    (else (export features-ok) (begin (define features-ok #f)))))
