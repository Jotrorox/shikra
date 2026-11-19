;; --bignum: number->string / string->number in radix 2, 8, 10 and 16 (round trips), integer literals
;; of any size in every radix, exact decimals (#e1e30, #e1.5e3), ratios of bignums (exact when they
;; divide, else the nearest flonum), inexact literals, printing a 10,000-digit number. Expected output:
;; tests/cases/inc/bignum_ref.py.
(import (scheme base) (scheme write) (scheme inexact))
(define (show . xs) (for-each (lambda (x) (write x) (display " ")) xs) (newline))
(define M 2305843009213693951)
(define (fdig d) (cond ((infinite? d) (if (> d 0) '+inf '-inf))
                       ((infinite? (* d 1e6)) (if (> d 0) '+inf '-inf))
                       (else (modulo (exact (floor (* d 1e6))) M))))
(define vals (list (expt 2 62) (- (- (expt 2 62)) 1) (- (expt 2 64) 1) (- (expt 2 64)) (expt 10 30) (expt 3 150)
                   (- (expt 7 77)) (- (expt 2 521) 1)
                   111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
                   (+ (expt 16 40) 1)))
(for-each (lambda (v)
            (for-each (lambda (r) (let ((s (number->string v r))) (show 'radix r s (= (string->number s r) v))))
                      '(2 8 10 16)))
          vals)
(show 'lit #x123456789abcdef0123456789 #b-1010101010101010101010101010101010101010101010101010101010101010101
      #o7777777777777777777777777 +123456789012345678901234567890 -123456789012345678901234567890)
(show 'lit0 000000000000123456789012345678901 #d#e123456789012345678901)
(show 'e-lit #e1e30 #e1.5e3 #e12345678901234567890.0 #e-1.0e25 (modulo #e1e400 M))
(show 's->n (string->number "#e1e30") (string->number "#e1.5e3") (string->number "#e1.2e-3") (string->number "#e1.5")
      (string->number "#x100000000000000000000") (string->number "-1" 10) (string->number "#x-100000000000000000000")
      (string->number "73786976294838206464/4") (string->number "#x10000000000000000/2"))
(show 'ratio (fdig 100000000000000000000/3) (fdig (string->number "-1180591620717411303424/7")))
(show 'inexact-lit (fdig #i1180591620717411303424) (fdig (string->number "#i1e400")))
(let* ((t (+ (expt 10 9999) 1)) (s (number->string t)) (n (string-length s)) (u (number->string (expt 3 20959))))
  (show 'big-print n (substring s 0 20) (substring s (- n 20) n) (= (string->number s) t)
        (string-length u) (string->number (substring u (- (string-length u) 12) (string-length u)))))
(display 'display) (display " ") (display (expt 2 100)) (display " ") (newline)
