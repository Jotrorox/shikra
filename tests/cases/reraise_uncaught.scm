;; A guard with no matching clause re-raises; with no outer handler the program reports the
;; raised object and exits with status 70 after flushing stdout. No after thunk runs.
(import (scheme base) (scheme write))
(display "start\n")
(dynamic-wind
 (lambda () #f)
 (lambda () (guard (e ((string? e) 'string)) (raise 'not-a-string)))
 (lambda () (display "after\n")))
