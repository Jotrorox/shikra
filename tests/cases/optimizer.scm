;; Programs that would catch optimizer bugs (M4-1): inlining with set! on captured variables,
;; shadowing after inlining, recursion that must not be inlined forever, effects in unused
;; arguments, fresh bindings per loop iteration, letrec, deep non-tail recursion, known calls
;; that become arity errors only at run time, tier-1 escapes from loops.
(import (scheme base) (scheme write))
(define (show . xs) (for-each (lambda (x) (write x) (display " ")) xs) (newline))

;; set! on a captured variable inside an inlined procedure
(define (make-counter)
  (let ((n 0))
    (lambda () (set! n (+ n 1)) n)))
(define c1 (make-counter))
(define c2 (make-counter))
(c1) (c1) (c2)
(show 'counters (c1) (c2))
(define (sum-with-set l)
  (let ((s 0))
    (for-each (lambda (x) (set! s (+ s x))) l)
    s))
(show 'sum (sum-with-set '(1 2 3 4)))
(define (apply-twice f x) (f (f x)))
(define (adder-box)
  (let ((total 0))
    (apply-twice (lambda (x) (set! total (+ total x)) (* x 2)) 5)
    total))
(show 'adder (adder-box))

;; shadowing: the inlined procedure's free variable is shadowed at the call site
(define scale 10)
(define (scaled x) (* x scale))
(define (f1 scale) (scaled scale))
(show 'shadow (f1 3) (let ((scale 100)) (scaled scale)))
(define (outer x)
  (define (helper y) (+ x y))
  (let ((x 1000)) (helper x)))
(show 'free (outer 1))
(define-syntax my-or (syntax-rules () ((_ a b) (let ((t a)) (if t t b)))))
(define (hyg t) (my-or #f t))
(show 'hygiene (hyg 7))

;; recursion is not inlined forever
(define (count-down n) (if (= n 0) 'done (count-down (- n 1))))
(define (ping n) (if (= n 0) 'ping (pong (- n 1))))
(define (pong n) (if (= n 0) 'pong (ping (- n 1))))
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(define (tree-sum t) (if (pair? t) (+ (tree-sum (car t)) (tree-sum (cdr t))) (if (number? t) t 0)))
(show 'recursion (count-down 100000) (ping 1001) (fact 20) (tree-sum '((1 2) (3 (4 5)) 6)))

;; effects in arguments whose parameter is unused happen once, in order
(define trace '())
(define (note x) (set! trace (cons x trace)) x)
(define (ignore-first a b) b)
(define (ignore-all a b c) 'none)
(show 'effects (ignore-first (note 1) (note 2)) (ignore-all (note 3) (note 4) (note 5)) (reverse trace))
(set! trace '())
(let ((r (list (note 'a) (note 'b) (note 'c)))) (show 'order r (reverse trace)))

;; each iteration of a loop gets a fresh binding (closures capture it)
(define procs
  (let loop ((i 0) (acc '()))
    (if (= i 5) (reverse acc) (loop (+ i 1) (cons (lambda () i) acc)))))
(show 'fresh (map (lambda (p) (p)) procs))
(define boxes
  (let loop ((i 0) (acc '()))
    (if (= i 3)
        (reverse acc)
        (let ((j i)) (loop (+ i 1) (cons (lambda () (set! j (+ j 10)) j) acc))))))
(show 'fresh-assigned (map (lambda (p) (p)) boxes) (map (lambda (p) (p)) boxes))
(define do-procs (do ((i 0 (+ i 1)) (acc '() (cons (lambda () (* i i)) acc))) ((= i 4) acc)))
(show 'do-fresh (map (lambda (p) (p)) do-procs))

;; letrec: mutual recursion, a lambda referenced before its group, delayed values
(define (parity n)
  (letrec ((ev? (lambda (k) (if (= k 0) #t (od? (- k 1)))))
           (od? (lambda (k) (if (= k 0) #f (ev? (- k 1))))))
    (list (ev? n) (od? n))))
(show 'letrec (parity 10) (parity 7))
(define (lr)
  (letrec* ((a 1) (f (lambda () (+ a b))) (b 2) (c (f)))
    (list a b c)))
(show 'letrec* (lr))
(define (state-machine s)
  (define (s0 l acc) (cond ((null? l) acc) ((eq? (car l) 'x) (s1 (cdr l) (+ acc 1))) (else (s0 (cdr l) acc))))
  (define (s1 l acc) (cond ((null? l) (- acc)) ((eq? (car l) 'y) (s0 (cdr l) (* acc 2))) (else (s1 (cdr l) acc))))
  (s0 s 0))
(show 'machine (state-machine '(a x b y x y x)))

;; deep non-tail recursion (direct C calls)
(define (build n) (if (= n 0) '() (cons n (build (- n 1)))))
(define (len l) (if (null? l) 0 (+ 1 (len (cdr l)))))
(show 'deep (len (build 100000)))

;; a known procedure called with the wrong count through a higher-order procedure is a run-time
;; error with the callee's name, as an unknown call
(define (two-args a b) (+ a b))
(define (call-with-one f) (f 1))
(show 'arity (guard (e (#t (error-object-message e))) (call-with-one two-args)))
(show 'car-error (guard (e (#t (list (error-object-message e) (error-object-irritants e)))) (car (cdr '(1)))))

;; tier-1 escapes out of loops and nested calls; escaping continuations stay tier 2
(define (find-first pred l) (call/cc (lambda (k) (for-each (lambda (x) (if (pred x) (k x))) l) #f)))
(show 'escape (find-first even? '(1 3 4 5)) (find-first even? '(1 3)))
(define saved #f)
(define (escaping) (+ 1 (call/cc (lambda (k) (set! saved k) 1))))
(show 'escaping (escaping))
(define (deep-escape n) (call/cc (lambda (out) (let loop ((i 0)) (if (= i n) (out 'reached) (loop (+ i 1)))))))
(show 'deep-escape (deep-escape 1000000))
(define (wind-escape)
  (let ((log '()))
    (call/cc (lambda (k) (dynamic-wind (lambda () (set! log (cons 'in log))) (lambda () (k 1)) (lambda () (set! log (cons 'out log))))))
    (reverse log)))
(show 'wind (wind-escape))

;; constants: top-level definitions never set! are constants; ones that are set! are not
(define limit 3)
(define mutable 1)
(define (read-mutable) mutable)
(set! mutable 2)
(show 'constants (+ limit 1) (read-mutable))
(define alias car)
(show 'alias (alias '(9 8)))
