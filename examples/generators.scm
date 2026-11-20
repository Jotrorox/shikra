;; generators.scm: generators and coroutines from call/cc.
;;
;;   shikra run generators.scm
;;
;; A generator here is a procedure that returns the next value of a computation each time it
;; is called, resuming the computation where it left off. That needs re-entrant continuations,
;; which Shikra compiles by copying the C stack; escape-only uses of call/cc stay as cheap as
;; a jump.
(import (scheme base) (scheme write))

;; (make-generator producer): producer is called with a yield procedure. Each call of the
;; generator returns the next yielded value, then the eof object forever.
(define (make-generator producer)
  (define return #f)
  (define resume #f)
  (define (yield v)
    (call/cc (lambda (k) (set! resume k) (return v))))
  (lambda ()
    (call/cc
     (lambda (r)
       (set! return r)
       (if resume
           (resume #f)
           (begin
             (producer yield)
             (set! resume (lambda (ignored) (return (eof-object))))
             (return (eof-object))))))))

;; The leaves of a tree, left to right, one per call.
(define (leaves tree)
  (make-generator
   (lambda (yield)
     (let walk ((t tree))
       (cond ((null? t))
             ((pair? t) (walk (car t)) (walk (cdr t)))
             (else (yield t)))))))

;; Same fringe: do two trees have the same leaves in the same order? The generators stop at
;; the first difference, without flattening either tree.
(define (same-fringe? a b)
  (let ((ga (leaves a)) (gb (leaves b)))
    (let loop ()
      (let ((x (ga)) (y (gb)))
        (cond ((and (eof-object? x) (eof-object? y)) #t)
              ((equal? x y) (loop))
              (else #f))))))

;; An infinite generator: the Fibonacci numbers.
(define (fibonacci)
  (make-generator
   (lambda (yield)
     (let loop ((a 0) (b 1))
       (yield a)
       (loop b (+ a b))))))

(define (take gen n)
  (if (= n 0) '() (let ((x (gen))) (cons x (take gen (- n 1))))))

;; Filtering and mapping generators compose like lists, but lazily.
(define (gen-filter keep? gen)
  (make-generator
   (lambda (yield)
     (let loop ()
       (let ((x (gen)))
         (unless (eof-object? x)
           (when (keep? x) (yield x))
           (loop)))))))

(write (same-fringe? '(1 (2 3) ((4)) 5) '((1 2) 3 (4 (5)))))
(newline)
(write (same-fringe? '(1 (2 3) 4) '(1 2 (4 3))))
(newline)
(write (take (fibonacci) 15))
(newline)
(write (take (gen-filter even? (fibonacci)) 10))
(newline)
