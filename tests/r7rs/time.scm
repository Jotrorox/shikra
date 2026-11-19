;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme time).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme time)
        (chibi test))

(test-begin "R7RS (scheme time)")

(test-begin "6.14 System interface")

(test #t (real? (current-second)))
(test #t (inexact? (current-second)))
(test #t (exact? (current-jiffy)))
(test #t (exact? (jiffies-per-second)))

(test-end)

(test-end)
