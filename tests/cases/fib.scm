(import (scheme base) (scheme write))

(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))

(display (fib 30))
(newline)
(display (map fib '(0 1 2 3 4 5 6 7 8 9 10)))
(newline)
