;; (scheme time) -- R7RS 6.14. current-second is POSIX time (UTC, not TAI) as a flonum;
;; jiffies are nanoseconds of the monotonic clock (an exact integer).
(define-library (scheme time)
  (import (shikra primitives))
  (export current-second current-jiffy jiffies-per-second)
  (begin
    (define (current-second) (%current-second))
    (define (current-jiffy) (%current-jiffy))
    (define (jiffies-per-second) 1000000000)))
