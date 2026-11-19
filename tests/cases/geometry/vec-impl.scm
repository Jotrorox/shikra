(define-record-type vec (make-vec x y) vec? (x vec-x) (y vec-y))
(define (vec+ a b) (make-vec (+ (vec-x a) (vec-x b)) (+ (vec-y a) (vec-y b))))
(define (vec-scale k v) (make-vec (* k (vec-x v)) (* k (vec-y v))))
