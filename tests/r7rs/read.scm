;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme read).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme read)
        (scheme write)
        (chibi test))

(test-begin "R7RS (scheme read)")

(test-begin "6.11 Exceptions")

(test #t
    (read-error? (guard (exn (else exn)) (read (open-input-string ")")))))
(test #t
    (read-error? (guard (exn (else exn)) (read (open-input-string "\"")))))

(test-end)

(test-begin "6.13 Input and output")

(test #t (eof-object? (read (open-input-string ""))))

(test 42 (read (open-input-string " 42 ")))

(test-begin "Read syntax")

;; check reading boolean followed by eof
(test #t (read (open-input-string "#t")))
(test #t (read (open-input-string "#true")))
(test #f (read (open-input-string "#f")))
(test #f (read (open-input-string "#false")))
(define (read2 port)
  (let* ((o1 (read port)) (o2 (read port)))
    (cons o1 o2)))
;; check reading boolean followed by delimiter
(test '(#t . (5)) (read2 (open-input-string "#t(5)")))
(test '(#t . 6) (read2 (open-input-string "#true 6 ")))
(test '(#f . 7) (read2 (open-input-string "#f 7")))
(test '(#f . "8") (read2 (open-input-string "#false\"8\"")))

(test '() (read (open-input-string "()")))
(test '(1 2) (read (open-input-string "(1 2)")))
(test '(1 . 2) (read (open-input-string "(1 . 2)")))
(test '(1 2) (read (open-input-string "(1 . (2))")))
(test '(1 2 3 4 5) (read (open-input-string "(1 . (2 3 4 . (5)))")))
(test '1 (cadr (read (open-input-string "#0=(1 . #0#)"))))
(test '(1 2 3) (cadr (read (open-input-string "(#0=(1 2 3) #0#)"))))
(test-error (read (open-input-string "#7=#23=#200#")))

(test '(quote (1 2)) (read (open-input-string "'(1 2)")))
(test '(quote (1 (unquote 2))) (read (open-input-string "'(1 ,2)")))
(test '(quote (1 (unquote-splicing 2))) (read (open-input-string "'(1 ,@2)")))
(test '(quasiquote (1 (unquote 2))) (read (open-input-string "`(1 ,2)")))

(test #() (read (open-input-string "#()")))
(test #(a b) (read (open-input-string "#(a b)")))

(test #u8() (read (open-input-string "#u8()")))
(test #u8(0 1) (read (open-input-string "#u8(0 1)")))

(test 'abc (read (open-input-string "abc")))
(test 'abc (read (open-input-string "abc def")))
(test 'ABC (read (open-input-string "ABC")))
(test 'Hello (read (open-input-string "|H\\x65;llo|")))

(test 'abc (read (open-input-string "#!fold-case ABC")))
(test 'ABC (read (open-input-string "#!fold-case #!no-fold-case ABC")))

(test 'def (read (open-input-string "#; abc def")))
(test 'def (read (open-input-string "; abc \ndef")))
(test 'def (read (open-input-string "#| abc |# def")))
(test 'ghi (read (open-input-string "#| abc #| def |# |# ghi")))
(test 'ghi (read (open-input-string "#; ; abc\n def ghi")))
(test '(abs -16) (read (open-input-string "(#;sqrt abs -16)")))
(test '(a d) (read (open-input-string "(a #; #;b c d)")))
(test '(a e) (read (open-input-string "(a #;(b #;c d) e)")))
(test '(a . c) (read (open-input-string "(a . #;b c)")))
(test '(a . b) (read (open-input-string "(a . b #;c)")))

(define (test-read-error str)
  (test-assert str
      (guard (exn (else #t))
        (read (open-input-string str))
        #f)))

(test-read-error "(#;a . b)")
(test-read-error "(a . #;b)")
(test-read-error "(a #;. b)")
(test-read-error "(#;x #;y . z)")
(test-read-error "(#; #;x #;y . z)")
(test-read-error "(#; #;x . z)")

(test #\a (read (open-input-string "#\\a")))
(test #\space (read (open-input-string "#\\space")))
(test 0 (char->integer (read (open-input-string "#\\null"))))
(test 7 (char->integer (read (open-input-string "#\\alarm"))))
(test 8 (char->integer (read (open-input-string "#\\backspace"))))
(test 9 (char->integer (read (open-input-string "#\\tab"))))
(test 10 (char->integer (read (open-input-string "#\\newline"))))
(test 13 (char->integer (read (open-input-string "#\\return"))))
(test #x7F (char->integer (read (open-input-string "#\\delete"))))
(test #x1B (char->integer (read (open-input-string "#\\escape"))))
(test #x03BB (char->integer (read (open-input-string "#\\λ"))))
(test #x03BB (char->integer (read (open-input-string "#\\x03BB"))))

(test "abc" (read (open-input-string "\"abc\"")))
(test "abc" (read (open-input-string "\"abc\" \"def\"")))
(test "ABC" (read (open-input-string "\"ABC\"")))
(test "Hello" (read (open-input-string "\"H\\x65;llo\"")))
(test 7 (char->integer (string-ref (read (open-input-string "\"\\a\"")) 0)))
(test 8 (char->integer (string-ref (read (open-input-string "\"\\b\"")) 0)))
(test 9 (char->integer (string-ref (read (open-input-string "\"\\t\"")) 0)))
(test 10 (char->integer (string-ref (read (open-input-string "\"\\n\"")) 0)))
(test 13 (char->integer (string-ref (read (open-input-string "\"\\r\"")) 0)))
(test #x22 (char->integer (string-ref (read (open-input-string "\"\\\"\"")) 0)))
(test #x7C (char->integer (string-ref (read (open-input-string "\"\\|\"")) 0)))
(test "line 1\nline 2\n" (read (open-input-string "\"line 1\nline 2\n\"")))
(test "line 1continued\n" (read (open-input-string "\"line 1\\\ncontinued\n\"")))
(test "line 1continued\n" (read (open-input-string "\"line 1\\ \ncontinued\n\"")))
(test "line 1continued\n" (read (open-input-string "\"line 1\\\n continued\n\"")))
(test "line 1continued\n" (read (open-input-string "\"line 1\\ \t \n \t continued\n\"")))
(test "line 1\n\nline 3\n" (read (open-input-string "\"line 1\\ \t \n \t \n\nline 3\n\"")))
(test #x03BB (char->integer (string-ref (read (open-input-string "\"\\x03BB;\"")) 0)))

(test-end)

(test-begin "Numeric syntax")

;; Numeric syntax adapted from Peter Bex's tests.
;;
;; These are updated to R7RS, using string ports instead of
;; string->number, and "error" tests removed because implementations
;; are free to provide their own numeric extensions.  Currently all
;; tests are run by default - need to cond-expand and test for
;; infinities and -0.0.

(define-syntax test-numeric-syntax
  (syntax-rules ()
    ((test-numeric-syntax str expect strs ...)
     (let* ((z (read (open-input-string str)))
            (out (open-output-string))
            (z-str (begin (write z out) (get-output-string out))))
       (test expect (values z))
       (test #t (and (member z-str '(str strs ...)) #t))))))

;; Each test is of the form:
;;
;;   (test-numeric-syntax input-str expected-value expected-write-values ...)
;;
;; where the input should be eqv? to the expected-value, and the
;; written output the same as any of the expected-write-values.  The
;; form
;;
;;   (test-numeric-syntax input-str expected-value)
;;
;; is a shorthand for
;;
;;   (test-numeric-syntax input-str expected-value (input-str))

;; Simple
(test-numeric-syntax "1" 1)
(test-numeric-syntax "+1" 1 "1")
(test-numeric-syntax "-1" -1)
(test-numeric-syntax "#i1" 1.0 "1.0" "1.")
(test-numeric-syntax "#I1" 1.0 "1.0" "1.")
(test-numeric-syntax "#i-1" -1.0 "-1.0" "-1.")
;; Decimal
(test-numeric-syntax "1.0" 1.0 "1.0" "1.")
(test-numeric-syntax "1." 1.0 "1.0" "1.")
(test-numeric-syntax ".1" 0.1 "0.1" "100.0e-3")
(test-numeric-syntax "-.1" -0.1 "-0.1" "-100.0e-3")
;; Some Schemes don't allow negative zero. This is okay with the standard
(test-numeric-syntax "-.0" -0.0 "-0." "-0.0" "0.0" "0." ".0")
(test-numeric-syntax "-0." -0.0 "-.0" "-0.0" "0.0" "0." ".0")
(test-numeric-syntax "#i1.0" 1.0 "1.0" "1.")
(test-numeric-syntax "#e1.0" 1 "1")
(test-numeric-syntax "#e-.0" 0 "0")
(test-numeric-syntax "#e-0." 0 "0")
;; Decimal notation with suffix
(test-numeric-syntax "1e2" 100.0 "100.0" "100.")
(test-numeric-syntax "1E2" 100.0 "100.0" "100.")
(test-numeric-syntax "1s2" 100.0 "100.0" "100.")
(test-numeric-syntax "1S2" 100.0 "100.0" "100.")
(test-numeric-syntax "1f2" 100.0 "100.0" "100.")
(test-numeric-syntax "1F2" 100.0 "100.0" "100.")
(test-numeric-syntax "1d2" 100.0 "100.0" "100.")
(test-numeric-syntax "1D2" 100.0 "100.0" "100.")
(test-numeric-syntax "1l2" 100.0 "100.0" "100.")
(test-numeric-syntax "1L2" 100.0 "100.0" "100.")
;; NaN, Inf
(test-numeric-syntax "+nan.0" +nan.0 "+nan.0" "+NaN.0")
(test-numeric-syntax "+NAN.0" +nan.0 "+nan.0" "+NaN.0")
(test-numeric-syntax "+inf.0" +inf.0 "+inf.0" "+Inf.0")
(test-numeric-syntax "+InF.0" +inf.0 "+inf.0" "+Inf.0")
(test-numeric-syntax "-inf.0" -inf.0 "-inf.0" "-Inf.0")
(test-numeric-syntax "-iNF.0" -inf.0 "-inf.0" "-Inf.0")
(test-numeric-syntax "#i+nan.0" +nan.0 "+nan.0" "+NaN.0")
(test-numeric-syntax "#i+inf.0" +inf.0 "+inf.0" "+Inf.0")
(test-numeric-syntax "#i-inf.0" -inf.0 "-inf.0" "-Inf.0")
;; Exact ratios

(test-numeric-syntax "10/2" 5 "5")

(test-numeric-syntax "0/10" 0 "0")
(test-numeric-syntax "#e0/10" 0 "0")
(test-numeric-syntax "#i3/2" (/ 3.0 2.0) "1.5")

;; Base prefixes
(test-numeric-syntax "#x11" 17 "17")
(test-numeric-syntax "#X11" 17 "17")
(test-numeric-syntax "#d11" 11 "11")
(test-numeric-syntax "#D11" 11 "11")
(test-numeric-syntax "#o11" 9 "9")
(test-numeric-syntax "#O11" 9 "9")
(test-numeric-syntax "#b11" 3 "3")
(test-numeric-syntax "#B11" 3 "3")
(test-numeric-syntax "#o7" 7 "7")
(test-numeric-syntax "#xa" 10 "10")
(test-numeric-syntax "#xA" 10 "10")
(test-numeric-syntax "#xf" 15 "15")
(test-numeric-syntax "#x-10" -16 "-16")
(test-numeric-syntax "#d-10" -10 "-10")
(test-numeric-syntax "#o-10" -8 "-8")
(test-numeric-syntax "#b-10" -2 "-2")
;; Combination of prefixes
(test-numeric-syntax "#e#x10" 16 "16")
(test-numeric-syntax "#i#x10" 16.0 "16.0" "16.")
(test-numeric-syntax "#x#i10" 16.0 "16.0" "16.")
(test-numeric-syntax "#i#x1/10" 0.0625 "0.0625")
(test-numeric-syntax "#x#i1/10" 0.0625 "0.0625")
;; (Attempted) decimal notation with base prefixes
(test-numeric-syntax "#d1." 1.0 "1.0" "1.")
(test-numeric-syntax "#d.1" 0.1 "0.1" ".1" "100.0e-3")
(test-numeric-syntax "#x1e2" 482 "482")
(test-numeric-syntax "#d1e2" 100.0 "100.0" "100.")
;; Fractions with prefixes
(test-numeric-syntax "#x10/2" 8 "8")

(test-end)

(test-end)

(test-end)
