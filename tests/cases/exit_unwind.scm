;; exit runs the outstanding dynamic-wind after thunks (innermost first), flushes the ports
;; and maps #f to status 1 (R7RS 6.14).
(import (scheme base) (scheme write) (scheme process-context))
(dynamic-wind
 (lambda () (display "outer in\n"))
 (lambda ()
   (dynamic-wind
    (lambda () (display "inner in\n"))
    (lambda () (display "exiting\n") (exit #f) (display "not reached\n"))
    (lambda () (display "inner out\n"))))
 (lambda () (display "outer out\n")))
