;; (scheme lazy) -- R7RS 4.2.5. delay and delay-force are expander keywords (expand.c):
;;   (delay-force e) = (%make-promise #f (lambda () e))
;;   (delay e)       = (delay-force (%make-promise #t e))
;; A promise holds a state pair (done? . value), or (#f . thunk) while pending. force follows
;; the reference implementation of R7RS: when the thunk returns another promise, that
;; promise's state is copied into this one and the two share one state pair from then on,
;; and the loop continues in tail position -- so a chain of delay-force runs in constant space.
;; A promise forced again from inside its own thunk keeps the first value that is delivered.
(define-library (scheme lazy)
  (import (shikra primitives))
  (export delay delay-force force make-promise promise?)
  (begin
    (define (promise? x) (%promise? x))
    (define (make-promise x) (if (%promise? x) x (%make-promise #t x)))
    (define (force p) (if (%promise? p) (force-promise p) p))
    (define (force-promise p)
      (let ((st (%promise-state p)))
        (if (%car st)
            (%cdr st)
            (let ((p2 ((%cdr st))))
              (if (%car (%promise-state p))
                  #f
                  (let ((st (%promise-state p)) (st2 (%promise-state p2)))
                    (%set-car! st (%car st2))
                    (%set-cdr! st (%cdr st2))
                    (%promise-set-state! p2 st)))
              (force-promise p)))))))
