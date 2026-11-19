;; dynamic-wind (R7RS 6.10; spec §8.4): escapes run the after thunks, innermost first, each
;; outside its own frame.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))
(define path '())
(define (add s) (set! path (cons s path)))
(define (take-path) (let ((p (reverse path))) (set! path '()) p))
(show (dynamic-wind (lambda () (add 'before)) (lambda () (add 'during) 'result) (lambda () (add 'after))))
(show (take-path))
;; R7RS 6.10 connect/talk/disconnect, escape-only variant (re-entry needs tier 3)
(let ((path '()))
  (let ((add (lambda (s) (set! path (cons s path)))))
    (show (call-with-current-continuation
           (lambda (k)
             (dynamic-wind
              (lambda () (add 'connect))
              (lambda () (add 'talk1) (k 'escaped) (add 'never))
              (lambda () (add 'disconnect))))))
    (show (reverse path))))
;; nested winds: an escape runs the inner after, then the outer one
(show (call/cc
       (lambda (k)
         (dynamic-wind
          (lambda () (add 'outer-in))
          (lambda ()
            (dynamic-wind
             (lambda () (add 'inner-in))
             (lambda () (k 'out))
             (lambda () (add 'inner-out))))
          (lambda () (add 'outer-out))))))
(show (take-path))
;; an escape from deep inside: only the winds between the call/cc and the escape run
(show (dynamic-wind
       (lambda () (add 'a-in))
       (lambda ()
         (call/cc (lambda (k)
                    (dynamic-wind (lambda () (add 'b-in)) (lambda () (k 'b-escaped)) (lambda () (add 'b-out))))))
       (lambda () (add 'a-out))))
(show (take-path))
;; raise caught by a guard outside: the after thunk runs before the guard clause body
(show (guard (e (#t (add (list 'clause e)) 'caught))
        (dynamic-wind (lambda () (add 'in)) (lambda () (raise 'err)) (lambda () (add 'out)))))
(show (take-path))
;; after thunks run with the dynamic environment outside their frame
(define p (make-parameter 'outside))
(call/cc (lambda (k)
           (dynamic-wind
            (lambda () (add (list 'before (p))))
            (lambda () (parameterize ((p 'inside)) (add (list 'body (p))) (k #f)))
            (lambda () (add (list 'after (p)))))))
(show (take-path))
;; the thunk may return several values
(show (call-with-values (lambda () (dynamic-wind (lambda () #f) (lambda () (values 1 2)) (lambda () #f))) list))
;; guard body escaping through nested guards and winds
(show (guard (e ((symbol? e) (add 'outer-clause) e))
        (dynamic-wind
         (lambda () (add 'w1-in))
         (lambda ()
           (guard (e ((string? e) 'no))
             (dynamic-wind (lambda () (add 'w2-in)) (lambda () (raise 'up)) (lambda () (add 'w2-out)))))
         (lambda () (add 'w1-out)))))
(show (take-path))
