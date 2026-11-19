;; Two nested named lets: both loops in one C function.
(import (scheme base) (scheme write))
(define (count-pairs n)
  (let outer ((i 0) (c 0))
    (if (= i n)
        c
        (let inner ((j 0) (c c))
          (if (= j n) (outer (+ i 1) c) (inner (+ j 1) (+ c 1)))))))
(display (count-pairs 100))
(newline)
