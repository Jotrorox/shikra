;; bignum.scm: exact integers of any size.
;;
;;   shikra run --bignum bignum.scm
;;   shikra run --bignum bignum.scm 1000   # digits of pi
;;
;; Without --bignum, exact integers are 63-bit fixnums and this program stops with an
;; overflow error at 21!. Only programs built with the flag carry the bignum code.
(import (scheme base) (scheme write) (scheme process-context))

(define (factorial n)
  (let loop ((i 2) (acc 1))
    (if (> i n) acc (loop (+ i 1) (* acc i)))))

(display "30! = ") (write (factorial 30)) (newline)
(display "digits in 1000! = ") (write (string-length (number->string (factorial 1000)))) (newline)
(display "2^200 in hex = ") (display (number->string (expt 2 200) 16)) (newline)
(call-with-values (lambda () (exact-integer-sqrt (+ (expt 10 60) 1)))
  (lambda (root rest)
    (display "sqrt of 10^60 + 1 = ") (write root)
    (display " remainder ") (write rest) (newline)))

;; Digits of pi with Machin's formula, pi/4 = 4 arctan(1/5) - arctan(1/239), in fixed point:
;; every number is scaled by 10^(digits + 10), and the 10 extra digits absorb rounding.
(define (arctan-inverse x unity)       ; arctan(1/x) * unity
  (let ((x2 (* x x)))
    (let loop ((term (quotient unity x)) (n 1) (sign 1) (sum 0))
      (if (zero? term)
          sum
          (loop (quotient term x2) (+ n 2) (- sign)
                (+ sum (* sign (quotient term n))))))))

(define (pi-digits digits)
  (let* ((unity (expt 10 (+ digits 10)))
         (pi (* 4 (- (* 4 (arctan-inverse 5 unity)) (arctan-inverse 239 unity)))))
    (quotient pi (expt 10 10))))

(define digits
  (let ((args (cdr (command-line))))
    (if (pair? args) (string->number (car args)) 100)))

(let ((s (number->string (pi-digits digits))))
  (display "pi = ")
  (display (string-append (substring s 0 1) "." (substring s 1 (string-length s))))
  (newline))
