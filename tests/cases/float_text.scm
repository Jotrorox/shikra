;; Float printing (shortest round trip) and parsing (correctly rounded) through number->string,
;; string->number, write and the reader (DESIGN.md "M5 as built"; the C-level corpus of 10^5
;; random doubles against libc is in tests/unit/rt_unit.c).
(import (scheme base) (scheme write) (scheme read) (scheme inexact) (util check))

(define (s x) (number->string x))
(define (n x) (string->number x))
(check (s (+ 0.1 0.2)) "0.30000000000000004")
(check (s (/ 1 3)) "0.3333333333333333")
(check (s (sqrt 2)) "1.4142135623730951")
(check (s 1e23) "1.0e+23")
(check (s 8.41e21) "8.41e+21")
(check (s 5e-324) "5.0e-324")
(check (s 2.2250738585072014e-308) "2.2250738585072014e-308")
(check (s 2.225073858507201e-308) "2.225073858507201e-308")   ; largest subnormal
(check (s 1.7976931348623157e308) "1.7976931348623157e+308")
(check (s (inexact 9007199254740993)) "9007199254740992.0")
(check (s 123456789012345680000.0) "123456789012345680000.0")
(check (s 1e21) "1.0e+21")
(check (s 1e-7) "0.0000001")
(check (s 1.5e-8) "1.5e-8")
(check (s -0.0) "-0.0")
(check (s 100.0) "100.0")
(check (s (exp 1)) "2.718281828459045")
(check (s (* 4 (atan 1))) "3.141592653589793")
;; parsing: halfway cases round to even, long inputs are exact
(check (n "9007199254740993") 9007199254740993)               ; exact integer
(check (n "9007199254740993.0") 9007199254740992.0)          ; halfway: to even
(check (n "9007199254740995.0") 9007199254740996.0)
(check (n "9007199254740993.0000000000000000000001") 9007199254740994.0)
(check (n "2.4703282292062327e-324") 0.0)                    ; just below half the smallest subnormal
(check (n "2.4703282292062328e-324") 5e-324)
(check (n "1e-400") 0.0)
(check (n "1e400") +inf.0)
(check (n "-1e400") -inf.0)
(check (n "2.2250738585072011e-308") 2.225073858507201e-308)
(check (n "0.1000000000000000055511151231257827021181583404541015625") 0.1)
(check (n "1.00000000000000011102230246251565404236316680908203125") 1.0)   ; exactly halfway: even
(check (n "1.00000000000000011102230246251565404236316680908203126") 1.0000000000000002)
(check (n (string-append "0." (make-string 400 #\0) "1e400")) 0.1)
(check (n (string-append "1" (make-string 1000 #\0) "e-1000")) 1.0)
(check (n "1s2") 100.0)
(check (n "1F-1") 0.1)
(check (n "#i1/3") (/ 1.0 3))
(check (n "#x10.5") #f)
;; every printed value reads back to itself
(define (round-trip? x) (eqv? x (n (s x))))
(check (let loop ((i 0) (x 1.0)) (cond ((= i 2000) #t) ((round-trip? x) (loop (+ i 1) (* x 1.0123456789))) (else x))) #t)
(check (let loop ((i 0) (x 1.0)) (cond ((= i 2000) #t) ((round-trip? x) (loop (+ i 1) (/ x 1.0987654321))) (else x))) #t)
(check (let loop ((i 1) (ok #t)) (if (= i 1000) ok (loop (+ i 1) (and ok (round-trip? (/ i 7.0)) (round-trip? (sqrt i)))))) #t)
;; the reader uses the same conversion
(check (read (open-input-string "0.30000000000000004")) (+ 0.1 0.2))
(check (let ((p (open-output-string))) (write (list 1.5 -2.0e-10 1e100) p) (get-output-string p)) "(1.5 -2.0e-10 1.0e+100)")
(check-done)
