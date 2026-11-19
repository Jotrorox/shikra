;; Re-entry inside parameterize, with-exception-handler and guard (tier 3): re-entering a
;; continuation restores the parameter bindings and the handler stack of its capture, and the
;; guard whose body is re-entered catches again (its own tier-2 frame came back with the copy).
(import (scheme base) (scheme write))
(define p (make-parameter 'outer))
(define k #f)
(define log '())
(define (note x) (set! log (cons x log)))
(define (show) (write (reverse log)) (newline) (set! log '()))

;; parameterize: the body sees its binding on every re-entry; outside it is undone
(define n 0)
(parameterize ((p 'inner))
  (note (list 'body (p) (call/cc (lambda (c) (set! k c) 0)))))
(note (list 'after (p)))
(set! n (+ n 1))
(if (< n 3) (parameterize ((p 'elsewhere)) (k n)))
(show)

;; a port parameter: output goes to the string port again after re-entry
(define kp #f)
(define sp (open-output-string))
(define count 0)
(parameterize ((current-output-port sp))
  (display (list 'line (call/cc (lambda (c) (set! kp c) 0)))))
(set! count (+ count 1))
(if (< count 3) (kp count))
(write (get-output-string sp))
(newline)

;; with-exception-handler: a raise-continuable after re-entry reaches the captured handler
(define kh #f)
(define m 0)
(note
 (with-exception-handler
  (lambda (e) (list 'handled e))
  (lambda ()
    (let ((v (call/cc (lambda (c) (set! kh c) 'first))))
      (raise-continuable v)))))
(set! m (+ m 1))
(if (< m 3) (with-exception-handler (lambda (e) 'wrong-handler) (lambda () (kh m))))
(show)

;; guard: re-entering the guard body and raising there is caught by that guard
(define kg #f)
(define g 0)
(note
 (guard (e ((symbol? e) (list 'caught e)))
   (let ((v (call/cc (lambda (c) (set! kg c) 'start))))
     (if (eq? v 'start) 'normal (raise v)))))
(set! g (+ g 1))
(if (= g 1) (kg 'boom))
(if (= g 2) (guard (e (#t (note 'outer-guard))) (kg 'bang)))
(show)

;; an error object raised inside re-entered code, caught by a guard around the re-entry
(define ke #f)
(define e-count 0)
(define r
  (guard (e ((error-object? e) (error-object-message e)))
    (+ 1 (call/cc (lambda (c) (set! ke c) 1)))))
(note r)
(set! e-count (+ e-count 1))
(if (= e-count 1) (ke 'not-a-number))
(show)
