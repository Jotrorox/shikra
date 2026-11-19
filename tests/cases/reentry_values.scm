;; Re-entry delivering multiple values (tier 3): the continuation of a call-with-values
;; producer and of a let-values binding receive 0, 1, 2 and 5 values on re-entry.
(import (scheme base) (scheme write))
(define k #f)
(define log '())
(call-with-values
 (lambda () (call/cc (lambda (c) (set! k c) (values 1 2))))
 (lambda args (set! log (cons args log))))
(case (length log)
  ((1) (k 'a 'b 'c 'd 'e))
  ((2) (k))
  ((3) (k 'single))
  ((4) (k 10 20)))
(write (reverse log))
(newline)

(define k2 #f)
(define seen '())
(let-values (((q r) (call/cc (lambda (c) (set! k2 c) (floor/ 17 5)))))
  (set! seen (cons (list q r) seen)))
(if (< (length seen) 3) (k2 (length seen) (* 10 (length seen))))
(write (reverse seen))
(newline)

;; a stored continuation called with values through apply
(define k3 #f)
(define got '())
(let ((r (call-with-values (lambda () (call/cc (lambda (c) (set! k3 c) (values 'x 'y)))) list)))
  (set! got (cons r got))
  (if (= (length got) 1) (apply k3 '(p q r))))
(write (reverse got))
(newline)
