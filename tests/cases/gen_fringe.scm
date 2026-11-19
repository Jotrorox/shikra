;; Generators on re-entrant call/cc (tier 3, spec §8.2): a tree walker that hands out one
;; leaf at a time, the classic same-fringe test, and a generator stored in a vector.
(import (scheme base) (scheme write))

;; (make-generator walk): walk is called with a yield procedure; each call of the generator
;; returns the next yielded value, then 'done forever.
(define (make-generator walk)
  (define return #f)
  (define resume #f)
  (define (yield v)
    (call/cc (lambda (k) (set! resume k) (return v))))
  (lambda ()
    (call/cc
     (lambda (r)
       (set! return r)
       (if resume
           (resume 'go)
           (begin (walk yield)
                  (set! resume (lambda (x) (return 'done)))
                  (return 'done)))))))

(define (tree-walker tree)
  (make-generator
   (lambda (yield)
     (let walk ((t tree))
       (cond ((null? t) 'skip)
             ((pair? t) (walk (car t)) (walk (cdr t)))
             (else (yield t)))))))

(define (same-fringe? a b)
  (let ((ga (tree-walker a)) (gb (tree-walker b)))
    (let loop ()
      (let ((x (ga)) (y (gb)))
        (cond ((not (eqv? x y)) #f)
              ((eq? x 'done) #t)
              (else (loop)))))))

(define (collect g)
  (let loop ((acc '()))
    (let ((x (g)))
      (if (eq? x 'done) (reverse acc) (loop (cons x acc))))))

(write (collect (tree-walker '((1 2) (3 (4 5)) 6))))
(newline)
(write (same-fringe? '(1 (2 3) ((4))) '((1 2) 3 4)))
(write (same-fringe? '(1 (2 3) 4) '(1 2 (4 3))))
(write (same-fringe? '() '(())))
(newline)

;; a generator of the integers, kept in a vector and called from elsewhere
(define gens (vector (make-generator (lambda (yield) (let loop ((i 0)) (yield i) (loop (+ i 1)))))))
(define (sum-first n)
  (let loop ((i 0) (s 0))
    (if (= i n) s (loop (+ i 1) (+ s ((vector-ref gens 0)))))))
(write (sum-first 100))
(write (sum-first 100))
(newline)
;; many values: each capture copies a small stack, the old copies are garbage
(define count-gen (make-generator (lambda (yield) (let loop ((i 0)) (yield i) (loop (+ i 1))))))
(let loop ((i 0) (last #f))
  (if (< i 20000)
      (loop (+ i 1) (count-gen))
      (begin (write last) (newline))))
