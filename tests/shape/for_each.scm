;; for-each with a lambda that assigns a captured variable: the loop is specialised to the
;; lambda (no closure, no unknown call) and the box of s disappears.
(import (scheme base) (scheme write))
(define (sum-list l)
  (let ((s 0))
    (for-each (lambda (x) (set! s (+ s x))) l)
    s))
(display (sum-list '(1 2 3 4 5)))
(newline)
