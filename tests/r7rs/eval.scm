;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme eval), REPL only.
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme eval)
        (chibi test))

(test-begin "R7RS (scheme eval)")

(test-begin "6.12 Environments and evaluation")

;; (test 21 (eval '(* 7 3) (scheme-report-environment 5)))

(test 1024 (eval '(expt 2 10) (environment '(scheme base))))
;; (sin 0) may return exact number
(test 0.0 (inexact (eval '(sin 0) (environment '(scheme inexact)))))
;; ditto
(test 1024.0 (eval '(+ (expt 2 10) (inexact (sin 0)))
                   (environment '(scheme base) '(scheme inexact))))

(test-end)

(test-end)
