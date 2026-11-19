;; Tier 1 (spec §8.2): the continuation is called from a lambda passed to for-each; after
;; inlining it is a jump out of the loop, with no runtime cost.
(import (scheme base) (scheme write))
(define (first-negative l)
  (call/cc
   (lambda (return)
     (for-each (lambda (x) (if (negative? x) (return x))) l)
     #f)))
(display (first-negative '(3 1 -4 1 -5)))
(newline)
