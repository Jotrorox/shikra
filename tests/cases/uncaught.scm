;; An uncaught error prints its message and irritants on stderr and exits with 70.
(import (scheme base) (scheme write))
(display "before the error")
(newline)
(error "Something bad happened:" 42 'foo "str")
(display "not reached")
