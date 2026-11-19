;; Exceptions (R7RS 4.2.7, 6.11; spec §8.3), directly on the handler stack: the runtime
;; list %handlers, innermost first. A handler runs with the stack of its
;; with-exception-handler minus itself. Runtime errors detected by primitives reach raise
;; through the engine (the compiled program's rt_engine_raise calls raise below).
(export raise raise-continuable error with-exception-handler guard
        error-object? error-object-message error-object-irritants
        file-error? read-error?)
(begin
  (define (with-exception-handler handler thunk)
    (let ((saved (%handlers)))
      (%set-handlers! (%cons handler saved))
      (let ((r (thunk)))
        (%set-handlers! saved)
        r)))
  (define (raise obj)
    (let ((hs (%handlers)))
      (if (%null? hs) (%uncaught obj))
      (%set-handlers! (%cdr hs))
      ((%car hs) obj)
      ;; the handler returned: a secondary exception in the handler's dynamic environment
      (error "exception handler returned from non-continuable raise:" obj)))
  (define (raise-continuable obj)
    (let ((hs (%handlers)))
      (if (%null? hs) (%uncaught obj))
      (%set-handlers! (%cdr hs))
      (let ((r ((%car hs) obj)))
        (%set-handlers! hs)
        r)))
  (define (error message . irritants)
    (raise (%make-error-object 0 message irritants)))
  ;; library-internal: "who: expected what" with the offending object as irritant
  (define (type-error who what obj)
    (raise (%make-error-object 0 (%string-append who (%string-append ": expected " what))
                               (%cons obj '()))))
  (define (error-object? x) (%error-object? x))
  (define (error-object-message e) (%error-object-message e))
  (define (error-object-irritants e) (%error-object-irritants e))
  (define (file-error? x) (%file-error? x))
  (define (read-error? x) (%read-error? x))

  ;; guard (spec §8.3): the clauses are evaluated on top of the raising frame, by the
  ;; handler that guard-run installs (so with the guard's handler stack). guard-select
  ;; returns #f when no clause matches -- the condition is then re-raised with
  ;; raise-continuable in the raiser's context -- or a thunk for the chosen clause's body,
  ;; which runs after unwinding (winders) and escaping to the guard (call/cc, tier 2).
  (define-syntax guard
    (syntax-rules ()
      ((_ (var clause ...) e1 e2 ...)
       (guard-run (lambda () e1 e2 ...)
                  (lambda (var) (guard-select clause ...))))))
  (define-syntax guard-select
    (syntax-rules (else =>)
      ((_) #f)
      ((_ (else e1 e2 ...)) (lambda () e1 e2 ...))
      ((_ (test => receiver) clause ...)
       (let ((t test)) (if t (lambda () (receiver t)) (guard-select clause ...))))
      ((_ (test) clause ...)
       (let ((t test)) (if t (lambda () t) (guard-select clause ...))))
      ((_ (test e1 e2 ...) clause ...)
       (if test (lambda () e1 e2 ...) (guard-select clause ...)))))
  (define guard-escape '(guard-escape)) ; unique: a literal nobody else can reach
  (define (guard-run thunk select)
    (let ((r (%call/cc
              (lambda (k)
                (let ((w (%winders)))
                  (with-exception-handler
                   (lambda (condition)
                     (let ((body (select condition)))
                       (if body
                           (begin (travel-to! w) (k (%cons guard-escape body)))
                           (raise-continuable condition))))
                   thunk))))))
      (if (%pair? r)
          (if (%eq? (%car r) guard-escape) ((%cdr r)) r)
          r))))
