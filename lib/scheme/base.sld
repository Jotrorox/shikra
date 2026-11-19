;; (scheme base) -- R7RS section 4 and 6, written on top of (shikra primitives).
;;
;; Each file under base/ holds the library declarations (export + begin) of one topic,
;; so topics grow independently. Wrappers follow DESIGN.md: (define (car x) (%car x));
;; optional arguments use case-lambda; variadic arithmetic folds the binary primitives.
;; M0 contains what the existing primitives support; M1 completes the library.
(define-library (scheme base)
  (import (shikra primitives) (shikra dynamic))
  (include-library-declarations
   "base/syntax.scm"
   "base/errors.scm"
   "base/equivalence.scm"
   "base/numbers.scm"
   "base/lists.scm"
   "base/symbols.scm"
   "base/chars.scm"
   "base/strings.scm"
   "base/vectors.scm"
   "base/bytevectors.scm"
   "base/control.scm"
   "base/io.scm"))
