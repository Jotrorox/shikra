;; Re-entering dynamic-wind bodies (tier 3, R7RS 6.10): each re-entry runs the before thunks
;; outermost first, each exit the after thunks innermost first; the trace shows the order.
;; Also: re-entry from inside another dynamic-wind (its after thunk runs first), an escape
;; out of nested winds with a tier-3 continuation, and a before thunk that re-enters.
(import (scheme base) (scheme write))
(define trace '())
(define (note x) (set! trace (cons x trace)))
(define (show) (write (reverse trace)) (newline) (set! trace '()))

;; re-enter a nested body three times
(define k #f)
(define n 0)
(dynamic-wind
 (lambda () (note 'in1))
 (lambda ()
   (dynamic-wind
    (lambda () (note 'in2))
    (lambda () (note (call/cc (lambda (c) (set! k c) 'first))))
    (lambda () (note 'out2))))
 (lambda () (note 'out1)))
(set! n (+ n 1))
(if (< n 4) (k n))
(show)

;; re-entry from inside another dynamic-wind: its after thunk runs before the re-entry's befores
(define k2 #f)
(define m 0)
(dynamic-wind
 (lambda () (note 'a-in))
 (lambda () (note (call/cc (lambda (c) (set! k2 c) 'a-body))))
 (lambda () (note 'a-out)))
(set! m (+ m 1))
(if (= m 1)
    (dynamic-wind
     (lambda () (note 'b-in))
     (lambda () (k2 'again))
     (lambda () (note 'b-out))))
(show)

;; a tier-3 continuation used to escape outwards through two winds
(define saved #f)
(define r
  (call/cc
   (lambda (out)
     (set! saved out)
     (dynamic-wind
      (lambda () (note 'x-in))
      (lambda () (dynamic-wind (lambda () (note 'y-in)) (lambda () (out 'escaped)) (lambda () (note 'y-out))))
      (lambda () (note 'x-out))))))
(note r)
(show)

;; generators interleaved with winds: a resumed producer re-enters its wind each time
(define resume #f)
(define back #f)
(define (produce)
  (dynamic-wind
   (lambda () (note 'p-in))
   (lambda ()
     (let loop ((i 0))
       (when (< i 3)
         (call/cc (lambda (c) (set! resume c) (back i)))
         (loop (+ i 1))))
     (back 'end))
   (lambda () (note 'p-out))))
(define (next!)
  (call/cc (lambda (b) (set! back b) (if resume (resume #f) (produce)))))
(let loop ()
  (let ((v (next!)))
    (note v)
    (if (not (eq? v 'end)) (loop))))
(show)

;; a before thunk that escapes while a re-entry rewinds: the re-entry is abandoned
(define k3 #f)
(define entries 0)
(define v
  (call/cc
   (lambda (outer)
     (dynamic-wind
      (lambda ()
        (set! entries (+ entries 1))
        (note (list 'enter entries))
        (if (= entries 2) (outer 'refused)))
      (lambda () (call/cc (lambda (c) (set! k3 c) 'inside)))
      (lambda () (note 'leave))))))
(note v)
(if (eq? v 'inside) (k3 'again))
(show)
