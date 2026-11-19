;; (scheme lazy) in constant space (R7RS 4.2.5, spec §3.1 table): long delay-force chains and
;; the stream examples of R7RS / SRFI 45 must not grow the stack (10^7 steps would overflow the C
;; stack or the interpreter's 2^25-slot stack otherwise); the chain also runs in constant heap.
;; Stream sizes are 10^5 so that GC stress stays fast (DESIGN.md "M5 as built", lazy).
(import (scheme base) (scheme write) (scheme lazy))

;; a 10^7-step delay-force chain
(define (chain n) (delay-force (if (= n 0) (delay 'done) (chain (- n 1)))))
(write (force (chain 10000000))) (newline)

;; R7RS 4.2.5: stream-filter over an infinite stream
(define integers
  (letrec ((next (lambda (n) (delay (cons n (next (+ n 1)))))))
    (next 0)))
(define (head s) (car (force s)))
(define (tail s) (cdr (force s)))
(define (stream-filter p? s)
  (delay-force
   (if (null? (force s))
       (delay '())
       (let ((h (car (force s))) (t (cdr (force s))))
         (if (p? h) (delay (cons h (stream-filter p? t))) (stream-filter p? t))))))
(define (stream-ref s k) (if (= k 0) (head s) (stream-ref (tail s) (- k 1))))
(write (head (tail (tail (stream-filter odd? integers))))) (newline)
(define (from n) (delay (cons n (from (+ n 1)))))
;; (each stream is made inside a procedure: a stream head left in a stale slot of a compiled
;; caller frame keeps the stream alive through the conservative stack scan; DESIGN.md known gaps)
(define (odd-ref k) (stream-ref (stream-filter odd? (from 0)) k))
(write (odd-ref 100000)) (newline)

;; SRFI 45 leak tests: (loop n) and a filter that skips 10^5 elements before the first match
(define (loop n) (delay-force (if (= n 0) (delay n) (loop (- n 1)))))
(write (force (loop 1000000))) (newline)
(define (first-match k) (head (stream-filter (lambda (n) (= n k)) (from 0))))
(write (first-match 100000)) (newline)

;; reentrancy (R7RS 4.2.5): a promise forced from inside its own thunk delivers the first value
(define count 0)
(define p
  (delay (begin (set! count (+ count 1))
                (if (> count 5) count (force p)))))
(write (list (force p) (force p) count)) (newline)
(define s (delay-force (begin (set! count 100) (delay count))))
(write (list (force s) (force s))) (newline)
(write (list (promise? (delay 1)) (promise? (make-promise 1)) (promise? 1)
             (force 7) (force (make-promise (delay 3))) (eq? (make-promise p) p)))
(newline)
;; the value of delay is not forced further: forcing (delay (delay 1)) gives a promise
(write (promise? (force (delay (delay 1))))) (newline)
(write (force (delay (values 1)))) (newline)
