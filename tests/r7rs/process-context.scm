;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme process-context).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme process-context)
        (chibi test))

(test-begin "R7RS (scheme process-context)")

(test-begin "6.14 System interface")

;; 6.14 System interface

;; (test "/usr/local/bin:/usr/bin:/bin" (get-environment-variable "PATH"))

(test #t (string? (get-environment-variable "PATH")))

;; (test '(("USER" . "root") ("HOME" . "/")) (get-environment-variables))

(let ((env (get-environment-variables)))
  (define (env-pair? x)
    (and (pair? x) (string? (car x)) (string? (cdr x))))
  (define (all? pred ls)
    (or (null? ls) (and (pred (car ls)) (all? pred (cdr ls)))))
  (test #t (list? env))
  (test #t (all? env-pair? env)))

(test #t (list? (command-line)))

(test-end)

(test-end)
