;; Exceptions (R7RS 4.2.7, 6.11; spec §8.3): handler stack, guard on top of the raising
;; frame, runtime errors of primitives raised through the same path.
(import (scheme base) (scheme write) (only (shikra primitives) %read %handlers))
(define (show x) (write x) (newline))
;; R7RS 6.11
(show (call-with-current-continuation
       (lambda (k)
         (with-exception-handler
          (lambda (e) (display "condition: ") (write e) (newline) (k 'exception))
          (lambda () (+ 1 (raise 'an-error)))))))
(show (with-exception-handler
       (lambda (con)
         (cond ((string? con) (display con))
               (else (display "a warning has been issued")))
         42)
       (lambda () (+ (raise-continuable "should be a number") 23))))
(define (null-list? l)
  (cond ((pair? l) #f)
        ((null? l) #t)
        (else (error "null-list?: argument out of domain" l))))
(show (guard (e ((error-object? e) (list (error-object-message e) (error-object-irritants e))))
        (null-list? 5)))
;; R7RS 4.2.7
(show (guard (condition ((assq 'a condition) => cdr) ((assq 'b condition)))
        (raise (list (cons 'a 42)))))
(show (guard (condition ((assq 'a condition) => cdr) ((assq 'b condition)))
        (raise (list (cons 'b 23)))))
;; guard: else, clause bodies, no raise, multiple values, internal definitions
(show (guard (e ((string? e) 'string) (else (list 'else e))) (raise 1)))
(show (guard (e (#t (display "clause ") 'two-expressions)) (raise 'x)))
(show (guard (e (#t 'unused)) 'no-raise))
(show (call-with-values (lambda () (guard (e (#t 0)) (values 1 2))) list))
(show (guard (e (#t e)) (define x 5) (raise (* x 2))))
;; runtime errors detected by primitives, when not provable at compile time
(define v (vector 5))
(show (guard (e ((error-object? e) (list (error-object-message e) (error-object-irritants e))))
        (car (vector-ref v 0))))
(show (guard (e ((error-object? e) (error-object-message e))) (vector-ref v 3)))
(show (guard (e ((error-object? e) (error-object-message e))) ((vector-ref (vector 5) 0) 1)))
(define (one x) x)
(show (guard (e ((error-object? e) (error-object-message e))) ((vector-ref (vector one) 0) 1 2)))
(show (guard (e ((error-object? e) (error-object-message e))) (+ (vector-ref (vector 4611686018427387903) 0) 1)))
(show (guard (e ((error-object? e) (error-object-message e)))
        (letrec ((a (lambda () b)) (b (a))) b)))
(show (guard (e ((read-error? e) 'read-error)) (%read (open-input-string "(1 2"))))
(show (guard (e ((file-error? e) 'file-error) ((error-object? e) 'other)) (error "plain")))
(show (list (error-object? 'x) (error-object? (guard (e (#t e)) (car '())))))
;; reraise: no clause matches, the outer guard gets the condition
(show (guard (outer (#t (list 'outer outer)))
        (guard (inner ((string? inner) 'inner)) (raise 'sym))))
(show (guard (e ((symbol? e) (list 'third e)))
        (guard (e ((string? e) 'second))
          (guard (e ((number? e) 'first)) (raise 'deep)))))
;; reraise with raise-continuable semantics: an outer handler's value returns to the raiser
(show (with-exception-handler
       (lambda (e) 10)
       (lambda () (+ 1 (guard (e ((string? e) 'no)) (raise-continuable 'x))))))
;; handlers run with the outer handler stack; handlers inside handlers
(show (with-exception-handler
       (lambda (e) (list 'outer e))
       (lambda ()
         (with-exception-handler
          (lambda (e) (raise-continuable (list 'inner e)))
          (lambda () (raise-continuable 'x))))))
(show (with-exception-handler
       (lambda (e) 'outermost)
       (lambda ()
         (with-exception-handler
          (lambda (e)
            (with-exception-handler
             (lambda (e2) (list 'nested e2))
             (lambda () (raise-continuable (list 'again e)))))
          (lambda () (raise-continuable 'first))))))
;; a clause test that raises goes to the guard's outer handlers
(show (guard (e (#t (list 'outer-caught e)))
        (guard (e ((raise 'from-test) 'never)) (raise 'original))))
;; a handler that returns from raise: secondary error in the outer handler's context
(show (guard (e ((error-object? e) (list (error-object-message e) (error-object-irritants e))))
        (with-exception-handler (lambda (e) 'ignored) (lambda () (raise 'boom)))))
;; 10^6 raises caught: constant stack, no handler-stack entries left behind
(define (count-raises n)
  (let loop ((i 0) (caught 0))
    (if (= i n)
        caught
        (loop (+ i 1) (+ caught (guard (e ((symbol? e) 1)) (raise 'x)))))))
(show (count-raises 1000000))
(show (length (%handlers)))
