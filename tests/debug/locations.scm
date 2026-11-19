;; -g (spec §11.3): an uncaught run-time error names the source location of the failing expression;
;; tests/run.sh debug runs this program once per argument 1..8 (locations.expected, one line each).
(import (scheme base) (scheme write) (scheme cxr) (scheme process-context))
(define (unknown s) (string->number s))   ; the analysis cannot see the value
(define v (vector 1 2 3))
(define (pick k)
  (case k
    ((1) (car (unknown "5")))                      ; type error in a primitive
    ((2) (vector-ref v (unknown "7")))             ; range error
    ((3) (+ (unknown "4611686018427387903") 1))    ; fixnum overflow
    ((4) ((vector-ref (vector (lambda (x) x)) (unknown "0")) 1 2)) ; arity of an unknown call
    ((5) (error "boom" k))                         ; user error
    ((6) (cadr (list (unknown "1"))))              ; inlined library wrapper
    ((7) (list-tail (list 1 2) (unknown "5")))     ; library procedure
    ((8) (string-ref "abc" (unknown "9")))))
(display "start")
(newline)
(pick (string->number (car (cdr (command-line)))))
