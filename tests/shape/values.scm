;; Multiple values (spec §8.5): a recursive procedure returning two values to known let-values
;; receivers returns a C struct; no values object is allocated.
(import (scheme base) (scheme write))
(define (divmod a b)
  (if (< a b)
      (values 0 a)
      (let-values (((q r) (divmod (- a b) b)))
        (values (+ q 1) r))))
(let-values (((q r) (divmod 100 7)))
  (display q) (display " ") (display r) (newline))
