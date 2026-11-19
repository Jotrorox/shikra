;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme case-lambda).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme case-lambda)
        (chibi test))

(test-begin "R7RS (scheme case-lambda)")

(test-begin "4.2 Derived expression types")

(define any-arity
  (case-lambda 
    (() 'zero)
    ((x) x)
    ((x y) (cons x y))
    ((x y z) (list x y z))
    (args (cons 'many args))))

(test 'zero (any-arity))
(test 1 (any-arity 1))
(test '(1 . 2) (any-arity 1 2))
(test '(1 2 3) (any-arity 1 2 3))
(test '(many 1 2 3 4) (any-arity 1 2 3 4))

(define rest-arity
  (case-lambda 
    (() '(zero))
    ((x) (list 'one x))
    ((x y) (list 'two x y))
    ((x y . z) (list 'more x y z))))

(test '(zero) (rest-arity))
(test '(one 1) (rest-arity 1))
(test '(two 1 2) (rest-arity 1 2))
(test '(more 1 2 (3)) (rest-arity 1 2 3))

(define dead-clause
  (case-lambda
    ((x . y) 'many)
    (() 'none)
    (foo 'unreachable)))

(test 'none (dead-clause))
(test 'many (dead-clause 1))
(test 'many (dead-clause 1 2))
(test 'many (dead-clause 1 2 3))

(test-end)

(test-end)
