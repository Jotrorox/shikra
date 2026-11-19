;; --bignum: products at Karatsuba sizes (above 40 limbs = 1,280 bits) up to 30,000 bits, balanced
;; and unbalanced, squares, and long division of those products (a*b + c) / b back to a and c. Each
;; line: index, digests modulo 2^61-1 of a*b, a*a, the quotient and the remainder, and whether they
;; reconstruct. Expected output: tests/cases/inc/bignum_ref.py.
(import (scheme base) (scheme write))
(define M 2305843009213693951)
(define seed 777)
(define (rnd) (set! seed (modulo (* seed 48271) 2147483647)) seed)
(define (rbits k)
  (let loop ((r 0) (b 0)) (if (< b k) (loop (+ (* r 2147483648) (rnd)) (+ b 31)) r)))
(do ((i 0 (+ i 1))) ((= i 120))
  (let* ((a (rbits (+ 1200 (modulo (rnd) 29000))))
         (b (rbits (+ 1200 (modulo (rnd) (if (even? i) 29000 4000)))))
         (a (if (= (modulo i 3) 0) (- a) a))
         (c (rbits (+ 1 (modulo (rnd) 1000))))
         (p (* a b)) (q (quotient (+ p c) b)) (r (remainder (+ p c) b)))
    (write i) (display " ") (write (modulo p M)) (display " ") (write (modulo (square a) M)) (display " ")
    (write (modulo q M)) (display " ") (write (modulo r M)) (display " ") (write (= (+ (* q b) r) (+ p c)))
    (newline)))
