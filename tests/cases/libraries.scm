;; User libraries in subdirectories (geometry/vec.sld, util/strings.sld): every import-set
;; form, export rename, include / include-ci / include-library-declarations, cond-expand in
;; library declarations and in the program, and a macro that keeps referring to a private
;; binding of its library.
(import (scheme base)
        (scheme write)
        (prefix (geometry vec) g:)
        (only (util strings) shout impl)
        (rename (only (util strings) loud features-ok) (loud the-loud))
        (except (scheme cxr) caddr)
        (only (prefix (rename (scheme base) (car first)) base:) base:first))
(cond-expand
  ((and shikra r7rs (not ratios)) (define where "shikra"))
  (else (define where "elsewhere")))
(cond-expand
  ((library (geometry vec)) (define have-vec #t))
  (else (define have-vec #f)))
(cond-expand
  ((library (no such lib)) (define have-none #t))
  (else (define have-none #f)))
(include "inc/helpers.scm")

(define v (g:vec+ (g:make-vec 1 2) (g:scale 10 (g:make-vec 3 4))))
(write (list (g:vec-x v) (g:vec-y v)))
(newline)
(write (g:with-double 21))
(newline)
(define double 'user-double)  ; does not capture the macro's private double
(write (list (g:with-double 5) double))
(newline)
(write (list (shout "hi") impl the-loud features-ok where have-vec have-none))
(newline)
(write (list (cadddr '(1 2 3 4)) (base:first '(a b)) (helper 4)))
(newline)
