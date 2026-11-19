;; emergency-exit does not run after thunks; output written so far is still flushed.
(import (scheme base) (scheme write) (scheme process-context))
(dynamic-wind
 (lambda () (display "in\n"))
 (lambda () (emergency-exit 3))
 (lambda () (display "after: not printed\n")))
