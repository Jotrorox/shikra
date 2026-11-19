;; exit with a status; output is flushed first.
(import (scheme base) (scheme write) (scheme process-context))
(display "exiting")
(newline)
(exit 3)
