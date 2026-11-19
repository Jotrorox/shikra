;; Collector: objects reachable only through a closure, a box (captured and assigned
;; variable), a record, a parameter (global value and parameterize binding), the
;; exception handler stack, a dynamic-wind frame, a tier-2 continuation frame, a values
;; object, an error object's irritants and a string port; each survives heavy allocation.
(import (scheme base) (scheme write))
(define (show x) (write x) (newline))
(define (churn) (let loop ((i 0) (acc '())) (if (= i 200000) (length acc) (loop (+ i 1) (if (= (modulo i 500) 0) '() (cons (vector i) acc))))))
(define (data tag) (list tag (vector tag 1 2) (string-append (symbol->string tag) "!")))
(define clo (let ((d (data 'closure))) (lambda () d)))
(define get-box (let ((d #f)) (set! d (data 'box)) (lambda () d)))
(define-record-type holder (make-holder v) holder? (v holder-v))
(define rec (make-holder (data 'record)))
(define param (make-parameter (data 'param)))
(define port (open-output-string))
(write-string "start:" port)
(churn)
(show (clo))
(show (get-box))
(show (holder-v rec))
(show (param))
(show (parameterize ((param (data 'bound))) (churn) (param)))
;; handler stack: the handler closes over data allocated only for it
(show (let ((d (data 'handler)))
        (with-exception-handler
         (lambda (e) (list e d))
         (lambda () (churn) (raise-continuable 'raised)))))
;; dynamic-wind after thunk holds data
(show (let* ((out #f)
             (r (dynamic-wind (lambda () #f)
                              (lambda () (churn) 'body)
                              (let ((d (data 'after))) (lambda () (set! out d))))))
        (list r out)))
;; tier-2 continuation: k is passed down; the frame keeps data live while callees churn
(define (descend k n) (if (= n 0) (begin (churn) (k (data 'escaped))) (+ 1 (descend k (- n 1)))))
(show (call/cc (lambda (k) (descend k 100))))
;; values object to an unknown receiver
(define receiver list)
(show (call-with-values (lambda () (let ((d (data 'values))) (churn) (values d (car d)))) receiver))
;; error object irritants
(show (guard (e (#t (churn) (error-object-irritants e))) (error "boom" (data 'irritant))))
;; string port contents built while churning
(let loop ((i 0)) (when (< i 50) (write i port) (churn) (loop (+ i 10))))
(show (get-output-string port))
