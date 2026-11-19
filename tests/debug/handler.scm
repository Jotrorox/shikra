;; -g with an exception handler in the program: the error object carries the location (it is
;; raised through (scheme base) raise, the handler declines, the error is uncaught).
(import (scheme base) (scheme write))
(define (unknown s) (string->number s))
(guard (e ((string? e) (display "caught")))
  (display (vector-ref (vector 1 2) (unknown "2"))))
