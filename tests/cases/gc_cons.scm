;; Collector: 10^7 conses with a live set of about 5,000 pairs (a ring of 1,000
;; five-element lists, overwritten in turn), plus short-lived garbage lists.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))
(define ring (make-vector 1000 '()))
(define (entry i) (list i (+ i 1) (+ i 2) (+ i 3) (+ i 4)))
(define (sum l acc) (if (null? l) acc (sum (cdr l) (+ acc (car l)))))
(define junk 0)
(let loop ((i 0))
  (when (< i 1500000)
    (vector-set! ring (modulo i 1000) (entry i))
    (set! junk (+ junk (sum (list 1 i 2) 0)))   ; three more conses, garbage at once
    (loop (+ i 1))))
;; every entry must still hold exactly its five numbers
(define (check-entry k)
  (let* ((l (vector-ref ring k)) (i (car l)))
    (and (= (modulo i 1000) k) (equal? l (entry i)))))
(show (let loop ((k 0)) (cond ((= k 1000) 'ring-ok) ((check-entry k) (loop (+ k 1))) (else (list 'bad k)))))
(show (let loop ((k 0) (acc 0)) (if (= k 1000) acc (loop (+ k 1) (sum (vector-ref ring k) acc)))))
(show junk)
