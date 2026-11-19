;; R7RS 6.11: a handler returning from a non-continuable raise causes a secondary exception
;; in the handler's dynamic environment; uncaught, it ends the program.
(import (scheme base) (scheme write))
(with-exception-handler
 (lambda (e) (display "something went wrong\n"))
 (lambda () (+ 1 (raise 'an-error))))
(display "not reached\n")
