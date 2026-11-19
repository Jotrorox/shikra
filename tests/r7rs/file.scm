;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme file).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme file)
        (chibi test))

(test-begin "R7RS (scheme file)")

(test-begin "6.11 Exceptions")

(test #t
    (file-error? (guard (exn (else exn)) (open-input-file " no such file "))))

(test-end)

(test-begin "6.14 System interface")

(test #t (file-exists? "."))
(test #f (file-exists? " no such file "))

(test #t (file-error?
          (guard (exn (else exn))
            (delete-file " no such file "))))

(test-end)

(test-end)
