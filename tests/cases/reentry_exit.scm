;; exit inside re-entered code (tier 3): exit runs the after thunks of the dynamic-winds that
;; are active in the re-entered continuation (not those of the stack it was called from), then
;; exits with the given status; output written before is flushed.
(import (scheme base) (scheme write) (scheme process-context))
(define k #f)
(define n 0)
(dynamic-wind
 (lambda () (display "in ") (display n) (newline))
 (lambda ()
   (call/cc (lambda (c) (set! k c)))
   (set! n (+ n 1))
   (when (= n 3)
     (display "exiting") (newline)
     (exit 7)))
 (lambda () (display "out ") (display n) (newline)))
(dynamic-wind
 (lambda () (display "elsewhere in") (newline))
 (lambda () (k #f))
 (lambda () (display "elsewhere out") (newline)))
