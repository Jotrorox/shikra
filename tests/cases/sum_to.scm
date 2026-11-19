;; The worked example of spec section 12.
(import (scheme base) (scheme write))

(define (sum-to n)
  (let loop ((i 0) (acc 0))
    (if (> i n)
        acc
        (loop (+ i 1) (+ acc i)))))

(display (sum-to 1000000))
(newline)
