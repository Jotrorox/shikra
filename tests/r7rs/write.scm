;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme write).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme write)
        (chibi test))

(test-begin "R7RS (scheme write)")

(test-begin "6.11 Exceptions")

;; Based on an example from R6RS-lib section 7.1 Exceptions.
;; R7RS section 6.11 Exceptions has a simplified version.
(let* ((out (open-output-string))
       (value (with-exception-handler
               (lambda (con)
                 (cond
                  ((not (list? con))
                   (raise con))
                  ((list? con)
                   (display (car con) out))
                  (else
                   (display "a warning has been issued" out)))
                 42)
               (lambda ()
                 (+ (raise-continuable
                     (list "should be a number"))
                    23)))))
  (test "should be a number" (get-output-string out))
  (test 65 value))

;; From SRFI-34 "Examples" section - #3
(define (test-exception-handler-3 v out)
  (guard (condition
          (else
           (display "condition: " out)
           (write condition out)
           (display #\! out)
           'exception))
         (+ 1 (if (= v 0) (raise 'an-error) (/ 10 v)))))
(let* ((out (open-output-string))
       (value (test-exception-handler-3 0 out)))
  (test 'exception value)
  (test "condition: an-error!" (get-output-string out)))

(define (test-exception-handler-4 v out)
  (call-with-current-continuation
   (lambda (k)
     (with-exception-handler
      (lambda (x)
        (display "reraised " out)
        (write x out) (display #\! out)
        (k 'zero))
      (lambda ()
        (guard (condition
                ((positive? condition)
                 'positive)
                ((negative? condition)
                 'negative))
          (raise v)))))))

;; From SRFI-34 "Examples" section - #5
(let* ((out (open-output-string))
       (value (test-exception-handler-4 1 out)))
  (test "" (get-output-string out))
  (test 'positive value))
;; From SRFI-34 "Examples" section - #6
(let* ((out (open-output-string))
       (value (test-exception-handler-4 -1 out)))
  (test "" (get-output-string out))
  (test 'negative value))
;; From SRFI-34 "Examples" section - #7
(let* ((out (open-output-string))
       (value (test-exception-handler-4 0 out)))
  (test "reraised 0!" (get-output-string out))
  (test 'zero value))

(test-end)

(test-begin "6.13 Input and output")

(test "abc"
    (let ((out (open-output-string)))
      (write 'abc out)
      (get-output-string out)))

(test "abc def"
    (let ((out (open-output-string)))
      (display "abc def" out)
      (get-output-string out)))

(test "abc"
    (let ((out (open-output-string)))
      (display #\a out)
      (display "b" out)
      (display #\c out)
      (get-output-string out)))

(test #t
    (and (member
          (let ((out (open-output-string))
                (x (list 1)))
            (set-cdr! x x)
            (write x out)
            (get-output-string out))
          ;; labels not guaranteed to be 0 indexed, spacing may differ
          '("#0=(1 . #0#)" "#1=(1 . #1#)"))
         #t))

(test "((1 2 3) (1 2 3))"
    (let ((out (open-output-string))
          (x (list 1 2 3)))
      (write (list x x) out)
      (get-output-string out)))

(test "((1 2 3) (1 2 3))"
    (let ((out (open-output-string))
          (x (list 1 2 3)))
      (write-simple (list x x) out)
      (get-output-string out)))

(test #t
    (and (member (let ((out (open-output-string))
                       (x (list 1 2 3)))
                   (write-shared (list x x) out)
                   (get-output-string out))
                 '("(#0=(1 2 3) #0#)" "(#1=(1 2 3) #1#)"))
         #t))

(test-begin "Read syntax")

(define-syntax test-write-syntax
  (syntax-rules ()
    ((test-write-syntax expect-str obj-expr)
     (let ((out (open-output-string)))
       (write obj-expr out)
       (test expect-str (get-output-string out))))))

(test-write-syntax "|.|" '|.|)
(test-write-syntax "|a b|" '|a b|)
(test-write-syntax "|,a|" '|,a|)
(test-write-syntax "|\"|" '|\"|)
(test-write-syntax "|\\||" '|\||)
(test-write-syntax "||" '||)
(test-write-syntax "|\\\\123|" '|\\123|)
(test-write-syntax "a" '|a|)
;; (test-write-syntax "a.b" '|a.b|)
(test-write-syntax "|2|" '|2|)
(test-write-syntax "|+3|" '|+3|)
(test-write-syntax "|-.4|" '|-.4|)
(test-write-syntax "|+i|" '|+i|)
(test-write-syntax "|-i|" '|-i|)
(test-write-syntax "|+inf.0|" '|+inf.0|)
(test-write-syntax "|-inf.0|" '|-inf.0|)
(test-write-syntax "|+nan.0|" '|+nan.0|)
(test-write-syntax "|+NaN.0|" '|+NaN.0|)
(test-write-syntax "|+NaN.0abc|" '|+NaN.0abc|)

(test-end)

(test-begin "Numeric syntax")

(define-syntax test-precision
  (syntax-rules ()
    ((test-round-trip str alt ...)
     (let* ((n (string->number str))
            (str2 (number->string n))
            (accepted (list str alt ...))
            (ls (member str2 accepted)))
       (test-assert (string-append "(member? " str2 " "
                                   (let ((out (open-output-string)))
                                     (write accepted out)
                                     (get-output-string out))
                                   ")")
         (pair? ls))
       (when (pair? ls)
         (test-assert (string-append "(eqv?: " str " " str2 ")")
           (eqv? n (string->number (car ls)))))))))

(test-precision "-1.7976931348623157e+308" "-inf.0")
(test-precision "4.940656458412465e-324" "4.94065645841247e-324" "5.0e-324" "0.0")
(test-precision "9.881312916824931e-324" "9.88131291682493e-324" "1.0e-323" "0.0")
(test-precision "1.48219693752374e-323" "1.5e-323" "0.0")
(test-precision "1.976262583364986e-323" "1.97626258336499e-323" "2.0e-323" "0.0")
(test-precision "2.470328229206233e-323" "2.47032822920623e-323" "2.5e-323" "0.0")
(test-precision "2.420921664622108e-322" "2.42092166462211e-322" "2.4e-322" "0.0")
(test-precision "2.420921664622108e-320" "2.42092166462211e-320" "2.421e-320" "0.0")
(test-precision "1.4489974452386991" "1.4489975")
(test-precision "0.14285714285714282" "0.14285714285714288" "0.14285715")
(test-precision "1.7976931348623157e+308" "+inf.0")

(test-end)

(test-end)

(test-end)
