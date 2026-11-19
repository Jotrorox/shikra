;; (scheme time) for Chez Scheme 10 -- see base.sls.
;; Jiffies are nanoseconds of the monotonic clock (small enough to stay a
;; fixnum); current-second is the UTC wall clock as a flonum.
(library (scheme time)
  (export current-second current-jiffy jiffies-per-second)
  (import (chezscheme))
  (define (current-second)
    (let ((t (current-time 'time-utc)))
      (+ (time-second t) (/ (time-nanosecond t) 1e9))))
  (define (current-jiffy)
    (let ((t (current-time 'time-monotonic)))
      (+ (* (time-second t) 1000000000) (time-nanosecond t))))
  (define (jiffies-per-second) 1000000000))
