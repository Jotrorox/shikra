;; The worked example of spec section 12, verbatim, built with --bignum: i stays an unboxed fixnum
;; (i + 1 cannot overflow, i <= n), acc may become a bignum and is a tagged obj added by rt_add.
(import (scheme base) (scheme write))

(define (sum-to n)
  (let loop ((i 0) (acc 0))
    (if (> i n)
        acc
        (loop (+ i 1) (+ acc i)))))

(display (sum-to 1000000))
(newline)
