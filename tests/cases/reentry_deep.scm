;; A continuation captured 10^4 frames deep (non-tail recursion) and re-entered 1,000 times:
;; from the top level (a shallower stack: the runtime grows the stack before copying back) and
;; from inside a deeper recursion. Each re-entry returns through all 10^4 frames again.
(import (scheme base) (scheme write))
(define k #f)
(define (deep n)
  (if (= n 0)
      (call/cc (lambda (c) (set! k c) 0))
      (+ 1 (deep (- n 1)))))
(define runs 0)
(define total 0)
(let ((r (deep 10000)))
  (set! runs (+ runs 1))
  (set! total (+ total r))
  (if (< runs 1000) (k runs)))
(write (list runs total))
(newline)
(set! k #f) ; the 10^4-frame copy becomes garbage

;; re-entry from deeper than the capture
(define k2 #f)
(define hits '())
(define (capture-at n) (if (= n 0) (call/cc (lambda (c) (set! k2 c) 'captured)) (let ((v (capture-at (- n 1)))) v)))
(define (descend n v) (if (= n 0) (k2 v) (+ 1 (descend (- n 1) v))))
(let ((v (capture-at 100)))
  (set! hits (cons v hits))
  (if (< (length hits) 4) (descend (* 3000 (length hits)) (length hits))))
(write (reverse hits))
(newline)
(set! k2 #f)

;; a continuation captured inside a deep non-tail map, re-entered: the earlier results stay
(define km #f)
(define big (let loop ((i 0) (acc '())) (if (= i 5000) acc (loop (+ i 1) (cons i acc)))))
(define first-pass #t)
(define result
  (map (lambda (x) (if (= x 2500) (call/cc (lambda (c) (set! km c) x)) x)) big))
(define sum (apply + result))
(if first-pass
    (begin (set! first-pass #f) (write sum) (newline) (km -100000))
    (begin (write sum) (newline)))
