;; syntax-rules: hygiene classics and every pattern/template feature.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))

;; my-or must not capture the user's t
(define-syntax my-or
  (syntax-rules ()
    ((_) #f)
    ((_ e) e)
    ((_ e r ...) (let ((t e)) (if t t (my-or r ...))))))
(define t 5)
(show (my-or #f t))
(show (let ((t 7)) (my-or #f t)))

;; swap! with a user variable named tmp
(define-syntax swap!
  (syntax-rules () ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))
(define tmp 1)
(define other 2)
(swap! tmp other)
(show (list tmp other))

;; the macro's if/list mean the binding at definition, even if the user shadows them
(define-syntax my-if
  (syntax-rules (then else)
    ((_ c then a else b) (if c a b))))
(show (let ((if list)) (my-if #t then 'yes else 'no)))
(show (let ((list vector)) (my-or #f (list 1))))

;; literals are compared by binding: a shadowed else is not the literal
(show (let ((else #f)) (cond (#f 1) (else 2))))
(show (cond (#f 1) (else 2)))

;; a macro-defining macro
(define-syntax define-getter
  (syntax-rules ()
    ((_ name value) (define-syntax name (syntax-rules () ((_) value))))))
(define-getter get-five 5)
(show (get-five))
(define-syntax def-incr
  (syntax-rules ()
    ((_ name) (define-syntax name
                (syntax-rules () ((_ v) (set! v (+ v 1))))))))
(def-incr incr!)
(define counter 10)
(incr! counter)
(show counter)

;; nested ellipsis, ellipsis following ellipsis, tail patterns, improper patterns
(define-syntax flatten2
  (syntax-rules () ((_ (a b ...) ...) '((a ...) (b ... ...)))))
(show (flatten2 (1 2 3) (4) (5 6)))
(define-syntax rotate
  (syntax-rules () ((_ a ... z) '(z a ...))))
(show (rotate 1 2 3 4))
(define-syntax last-two
  (syntax-rules () ((_ a ... y z) '(y z))))
(show (last-two 1 2 3))
(define-syntax dotted
  (syntax-rules () ((_ a . rest) '(a rest))))
(show (dotted 1 2 3))
(define-syntax dotted-tail
  (syntax-rules () ((_ a ... . r) '((a ...) r))))
(show (dotted-tail 1 2 . 3))

;; vector patterns and templates, underscore, custom ellipsis, (... ...)
(define-syntax vec-pat
  (syntax-rules () ((_ #(a b ...)) (list 'a a '#(b ...)))))
(show (vec-pat #(1 2 3)))
(define-syntax second
  (syntax-rules () ((_ _ x . _) x)))
(show (second 1 2 3 4))
(define-syntax my-list
  (syntax-rules ::: () ((_ x :::) (list x :::))))
(show (my-list 1 2 3))
(define-syntax quote-ellipsis
  (syntax-rules () ((_ x) '(x (... ...)))))
(show (quote-ellipsis 7))
(define-syntax lit-ellipsis
  (syntax-rules ... (...) ((_ x) '(x ...))))
(show (lit-ellipsis 100))
(define-syntax be-like-begin
  (syntax-rules ()
    ((be-like-begin name)
     (define-syntax name
       (syntax-rules ()
         ((name expr (... ...))
          (begin expr (... ...))))))))
(be-like-begin sequence)
(show (sequence 1 2 3 4))

;; let-syntax, letrec-syntax, internal define-syntax
(show (let-syntax ((foo (syntax-rules () ((_ x) (* x 10))))) (foo 4)))
(show (letrec-syntax ((ev? (syntax-rules () ((_) #t) ((_ x . r) (od? . r))))
                      (od? (syntax-rules () ((_) #f) ((_ x . r) (ev? . r)))))
        (list (ev? 1 2) (ev? 1 2 3))))
(show (let ((x 'outer))
        (let-syntax ((m (syntax-rules () ((_) x))))
          (let ((x 'inner))
            (m)))))
(define (internal)
  (define-syntax double (syntax-rules () ((_ e) (* 2 e))))
  (define y 21)
  (double y))
(show (internal))

;; a macro expanding into definitions (top level and body)
(define-syntax define-pair
  (syntax-rules () ((_ a b va vb) (begin (define a va) (define b vb)))))
(define-pair p1 p2 'first 'second)
(show (list p1 p2))
(show (let () (define-pair x y 1 2) (+ x y)))

;; recursive macro with literals (a tiny loop language)
(define-syntax for
  (syntax-rules (in from to)
    ((_ x in lst body ...) (for-each (lambda (x) body ...) lst))
    ((_ x from a to b body ...) (do ((x a (+ x 1))) ((> x b)) body ...))))
(for x in '(1 2 3) (display x))
(for i from 4 to 6 (display i))
(newline)
