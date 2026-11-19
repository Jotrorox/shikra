;; exit with #t (or no argument) is a normal exit; a non-integer non-boolean is too.
(import (scheme base) (scheme write) (scheme process-context))
(dynamic-wind (lambda () #f) (lambda () (exit #t)) (lambda () (display "unwound\n")))
