;; -*- coding: utf-8 -*-
;;
;; Shikra R7RS conformance suite: tests for (scheme inexact).
;; Generated from Chibi Scheme's tests/r7rs-tests.scm (see upstream/README)
;; by splitting it per library and adapting it to Shikra's documented
;; implementation restrictions.  Every removed or rewritten test is listed
;; in EXCLUSIONS.md.  Test order and text are otherwise upstream's.

(import (scheme base)
        (scheme inexact)
        (chibi test))

(test-begin "R7RS (scheme inexact)")

(test-begin "4.2 Derived expression types")

;; By Jussi Piitulainen <jpiitula@ling.helsinki.fi>
;; and John Cowan <cowan@mercury.ccil.org>:
;; http://lists.scheme-reports.org/pipermail/scheme-reports/2013-December/003876.html
(define (means ton)
  (letrec*
     ((mean
        (lambda (f g)
          (f (/ (sum g ton) n))))
      (sum
        (lambda (g ton)
          (if (null? ton)
            (+)
            (if (number? ton)
                (g ton)
                (+ (sum g (car ton))
                   (sum g (cdr ton)))))))
      (n (sum (lambda (x) 1) ton)))
    (values (mean values values)
            (mean exp log)
            (mean / /))))
(let*-values (((a b c) (means '(8 5 99 1 22))))
  (test 27 a)
  (test 9.728 b)
  (test 3.6217303822937628 c))  ; shikra: rewritten, see EXCLUSIONS.md

(test-end)

(test-begin "6.2 Numbers")

(test #t (finite? 3))
(test #f (finite? +inf.0))

(test #f (infinite? 3))
(test #t (infinite? +inf.0))
(test #f (infinite? +nan.0))

(test #t (nan? +nan.0))
(test #f (nan? 32))

(test 1.0 (inexact (exp 0))) ;; may return exact number
(test 20.0855369231877 (exp 3))

(test 0.0 (inexact (log 1))) ;; may return exact number
(test 1.0 (log (exp 1)))
(test 42.0 (log (exp 42)))
(test 2.0 (log 100 10))
(test 12.0 (log 4096 2))

(test 0.0 (inexact (sin 0))) ;; may return exact number
(test 1.0 (sin 1.5707963267949))
(test 1.0 (inexact (cos 0))) ;; may return exact number
(test -1.0 (cos 3.14159265358979))
(test 0.0 (inexact (tan 0))) ;; may return exact number
(test 1.5574077246549 (tan 1))

(test 0.0 (inexact (asin 0))) ;; may return exact number
(test 1.5707963267949 (asin 1))
(test 0.0 (inexact (acos 1))) ;; may return exact number
(test 3.14159265358979 (acos -1))

;; (test 0.0-0.0i (asin 0+0.0i))
;; (test 1.5707963267948966+0.0i (acos 0+0.0i))

(test 0.0 (atan 0.0 1.0))
(test -0.0 (atan -0.0 1.0))
(test 0.785398163397448 (atan 1.0 1.0))
(test 1.5707963267949 (atan 1.0 0.0))
(test 2.35619449019234 (atan 1.0 -1.0))
(test 3.14159265358979 (atan 0.0 -1.0))
(test -3.14159265358979 (atan -0.0 -1.0)) ;
(test -2.35619449019234 (atan -1.0 -1.0))
(test -1.5707963267949 (atan -1.0 0.0))
(test -0.785398163397448 (atan -1.0 1.0))
;; (test undefined (atan 0.0 0.0))

(test 3.0 (inexact (sqrt 9)))
(test 1.4142135623731 (sqrt 2))

(test-end)

(test-begin "6.8 Vectors")

(test 13 (vector-ref '#(1 1 2 3 5 8 13 21)
            (let ((i (round (* 2 (acos -1)))))
              (if (inexact? i)
                  (exact i)
                  i))))

(test-end)

(test-begin "6.11 Exceptions")

(test 'caught-d
    (guard (condition
            ((assq 'c condition) 'caught-c)
            ((assq 'd condition) 'caught-d))
      (list
       (sqrt 8)
       (guard (condition
               ((assq 'a condition) => cdr)
               ((assq 'b condition)))
         (raise (list (cons 'd 24)))))))

(test-end)

(test-end)
