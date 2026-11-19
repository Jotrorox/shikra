(define loaded-value 'from-loadme)
(define-syntax swap!
  (syntax-rules () ((_ a b) (let ((t a)) (set! a b) (set! b t)))))
(define (twice f x) (f (f x)))
