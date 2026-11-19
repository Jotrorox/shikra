;; Mutually recursive local procedures (a state machine): join points of one C function.
(import (scheme base) (scheme write))
(define (parity n)
  (define (ev? k) (if (= k 0) 'even (od? (- k 1))))
  (define (od? k) (if (= k 0) 'odd (ev? (- k 1))))
  (ev? n))
(display (parity 1000001))
(newline)
