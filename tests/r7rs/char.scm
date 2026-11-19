;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme char).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme char)
        (chibi test))

(test-begin "R7RS (scheme char)")

(test-begin "6.4 Lists")

(test '("b" "c") (member "B" '("a" "b" "c") string-ci=?))

(test-end)

(test-begin "6.6 Characters")

(test #t (char-ci=? #\a #\a))
(test #t (char-ci=? #\a #\A #\a))
(test #f (char-ci=? #\a #\b))
(test #t (char-ci<? #\a #\B #\c))
(test #f (char-ci<? #\A #\a))
(test #f (char-ci<? #\b #\A))
(test #f (char-ci>? #\A #\b))
(test #f (char-ci>? #\a #\A))
(test #t (char-ci>? #\c #\B #\a))
(test #t (char-ci<=? #\a #\B #\b))
(test #t (char-ci<=? #\A #\a))
(test #f (char-ci<=? #\b #\A))
(test #f (char-ci>=? #\A #\b))
(test #t (char-ci>=? #\a #\A))
(test #t (char-ci>=? #\b #\B #\a))

(test #t (char-alphabetic? #\a))
(test #f (char-alphabetic? #\space))
(test #t (char-numeric? #\0))
(test #f (char-numeric? #\.))
(test #f (char-numeric? #\a))
(test #t (char-whitespace? #\space))
(test #t (char-whitespace? #\tab))
(test #t (char-whitespace? #\newline))
(test #f (char-whitespace? #\_))
(test #f (char-whitespace? #\a))
(test #t (char-upper-case? #\A))
(test #f (char-upper-case? #\a))
(test #f (char-upper-case? #\3))
(test #t (char-lower-case? #\a))
(test #f (char-lower-case? #\A))
(test #f (char-lower-case? #\3))

(test 0 (digit-value #\0))
(test 3 (digit-value #\3))
(test 9 (digit-value #\9))

(test #f (digit-value #\.))
(test #f (digit-value #\-))

(test #\A (char-upcase #\a))
(test #\A (char-upcase #\A))
(test #\a (char-downcase #\a))
(test #\a (char-downcase #\A))
(test #\a (char-foldcase #\a))
(test #\a (char-foldcase #\A))

(test-end)

(test-begin "6.7 Strings")

(test #t (string-ci=? "" ""))
(test #t (string-ci=? "abc" "abc"))
(test #f (string-ci=? "" "abc"))
(test #t (string-ci=? "abc" "aBc"))
(test #f (string-ci=? "abc" "aBcD"))

(test #f (string-ci<? "abc" "aBc"))
(test #t (string-ci<? "abc" "aBcD"))
(test #f (string-ci<? "ABCd" "aBc"))

(test #f (string-ci>? "abc" "aBc"))
(test #f (string-ci>? "abc" "aBcD"))
(test #t (string-ci>? "ABCd" "aBc"))

(test #t (string-ci<=? "abc" "aBc"))
(test #t (string-ci<=? "abc" "aBcD"))
(test #f (string-ci<=? "ABCd" "aBc"))

(test #t (string-ci>=? "abc" "aBc"))
(test #f (string-ci>=? "abc" "aBcD"))
(test #t (string-ci>=? "ABCd" "aBc"))

;; latin
(test "ABC" (string-upcase "abc"))
(test "ABC" (string-upcase "ABC"))
(test "abc" (string-downcase "abc"))
(test "abc" (string-downcase "ABC"))
(test "abc" (string-foldcase "abc"))
(test "abc" (string-foldcase "ABC"))

(test-end)

(test-begin "6.10 Control Features")

(test "abdegh" (string-map char-foldcase "AbdEgH"))

(test "StUdLyCaPs"
    (string-map
     (lambda (c k) (if (eqv? k #\u) (char-upcase c) (char-downcase c)))
     "studlycaps xxx"
     "ululululul"))

(test-end)

(test-end)
