;; streams.scm: lazy infinite lists with delay and force.
;;
;;   shikra run streams.scm
;;
;; Shows: (scheme lazy), delay-force for streams that run in constant space, and macros
;; with syntax-rules.
(import (scheme base) (scheme lazy) (scheme write))

;; A stream is a promise of either '() or a pair whose cdr is a stream.
(define-syntax stream-cons
  (syntax-rules ()
    ((_ a b) (delay (cons a (delay-force b))))))

(define (stream-car s) (car (force s)))
(define (stream-cdr s) (cdr (force s)))

(define (integers-from n)
  (stream-cons n (integers-from (+ n 1))))

(define (stream-filter keep? s)
  (delay-force
   (let ((p (force s)))
     (cond ((null? p) (delay '()))
           ((keep? (car p)) (stream-cons (car p) (stream-filter keep? (cdr p))))
           (else (stream-filter keep? (cdr p)))))))

(define (stream-map f s)
  (delay-force
   (let ((p (force s)))
     (if (null? p) (delay '()) (stream-cons (f (car p)) (stream-map f (cdr p)))))))

(define (stream-take s n)
  (if (= n 0) '() (cons (stream-car s) (stream-take (stream-cdr s) (- n 1)))))

(define (stream-ref s n)
  (if (= n 0) (stream-car s) (stream-ref (stream-cdr s) (- n 1))))

;; The primes, by trial division against the primes found so far.
(define primes
  (stream-cons 2 (stream-filter prime? (integers-from 3))))

(define (prime? n)
  (let loop ((ps primes))
    (let ((p (stream-car ps)))
      (cond ((> (* p p) n) #t)
            ((= 0 (remainder n p)) #f)
            (else (loop (stream-cdr ps)))))))

(write (stream-take primes 20))
(newline)

;; The 10,000th prime: the streams are memoized, so the 10,000 primes are computed once.
(write (stream-ref primes 9999))
(newline)

;; Squares of the twin-prime pairs' first members.
(define (twin-starts s)
  (delay-force
   (let ((a (stream-car s)) (b (stream-car (stream-cdr s))))
     (if (= (- b a) 2)
         (stream-cons a (twin-starts (stream-cdr s)))
         (twin-starts (stream-cdr s))))))
(write (stream-take (stream-map (lambda (p) (* p p)) (twin-starts primes)) 8))
(newline)

;; A filter that skips a million elements before its first match still runs in constant
;; space: that is what delay-force is for.
(write (stream-car (stream-filter (lambda (n) (> n 1000000)) (integers-from 0))))
(newline)
