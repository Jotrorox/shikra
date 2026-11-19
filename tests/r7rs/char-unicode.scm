;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme char), needs --unicode=full.
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme char)
        (chibi test))

(test-begin "R7RS (scheme char)")

(test-begin "6.6 Characters")

(test #t (char-alphabetic? #\Λ))
(test #f (char-alphabetic? #\x0E50))
(test #t (char-upper-case? #\Λ))
(test #f (char-upper-case? #\λ))
(test #f (char-lower-case? #\Λ))
(test #t (char-lower-case? #\λ))
(test #f (char-numeric? #\Λ))
(test #t (char-numeric? #\x0E50))
(test #t (char-whitespace? #\x1680))

(test 4 (digit-value #\x0664))
(test 0 (digit-value #\x0AE6))

(test #\Λ (char-upcase #\λ))
(test #\Λ (char-upcase #\Λ))
(test #\λ (char-downcase #\λ))
(test #\λ (char-downcase #\Λ))
(test #\λ (char-foldcase #\λ))
(test #\λ (char-foldcase #\Λ))

(test-end)

(test-begin "6.7 Strings")

(test #t (string-ci=? "ΑΒΓ" "αβγ" "αβγ"))
(test #f (string-ci<? "ΑΒΓ" "αβγ"))
(test #f (string-ci>? "ΑΒΓ" "αβγ"))
(test #t (string-ci<=? "ΑΒΓ" "αβγ"))
(test #t (string-ci>=? "ΑΒΓ" "αβγ"))

;; cyrillic
(test "ΑΒΓ" (string-upcase "αβγ"))
(test "ΑΒΓ" (string-upcase "ΑΒΓ"))
(test "αβγ" (string-downcase "αβγ"))
(test "αβγ" (string-downcase "ΑΒΓ"))
(test "αβγ" (string-foldcase "αβγ"))
(test "αβγ" (string-foldcase "ΑΒΓ"))

;; special cases
(test "SSA" (string-upcase "ßa"))
(test "ßa" (string-downcase "ßa"))
(test "ssa" (string-downcase "SSA"))
(test "maß" (string-downcase "Maß"))
(test "mass" (string-foldcase "Maß"))
(test "İ" (string-upcase "İ"))
(test "i\x0307;" (string-downcase "İ"))
(test "i\x0307;" (string-foldcase "İ"))
(test "J̌" (string-upcase "ǰ"))
(test "ſ" (string-downcase "ſ"))
(test "s" (string-foldcase "ſ"))

;; context-sensitive (final sigma)
(test "ΓΛΏΣΣΑ" (string-upcase "γλώσσα"))
(test "γλώσσα" (string-downcase "ΓΛΏΣΣΑ"))
(test "γλώσσα" (string-foldcase "ΓΛΏΣΣΑ"))
(test "ΜΈΛΟΣ" (string-upcase "μέλος"))
(test #t (and (member (string-downcase "ΜΈΛΟΣ") '("μέλος" "μέλοσ")) #t))
(test "μέλοσ" (string-foldcase "ΜΈΛΟΣ"))
(test #t (and (member (string-downcase "ΜΈΛΟΣ ΕΝΌΣ")
                      '("μέλος ενός" "μέλοσ ενόσ"))
              #t))

(test-end)

(test-end)
