;; (chibi test) -- a small, portable re-implementation for the Shikra
;; conformance suite.
;;
;; Pure R7RS-small, syntax-rules only.  It provides the subset of Alex
;; Shinn's (chibi test) that tests/r7rs/*.scm use:
;;
;;   (test [name] expected expr)      compare with test-equal? (below)
;;   (test-assert [name] expr)        pass iff expr returns a true value
;;   (test-values [name] expected expr)  compare all returned values
;;   (test-error [name] expr)         pass iff evaluating expr raises
;;   (test-begin [name]) / (test-end [name])   open / close a group
;;   (test-group name body ...)       test-begin + body + test-end
;;   test-equal?                      the comparison procedure
;;
;; Semantics
;; - Every test catches anything raised while evaluating its expected and
;;   actual expressions; an unexpected raise counts as a failure (reported
;;   as ERROR) and the run continues.
;; - test-equal? is equal?, except that an inexact real expected value
;;   matches an inexact real result within a relative epsilon of 1e-5
;;   (absolute 1e-5 when the expected value is zero), and NaN matches NaN.
;;   The comparison recurses through pairs and vectors.  Exactness must
;;   match: an inexact expected value never matches an exact result and
;;   vice versa (this is stricter than upstream (chibi test), which lets an
;;   exact result satisfy an inexact expectation; Shikra's rewritten
;;   rational tests rely on the strict rule).
;; - Each test-end prints a one-line summary of its group.  Each failure
;;   prints the source expression, the expected and the actual value.
;; - The test-end that closes the outermost group prints a final line of
;;   exactly the form "N tests, P passed, F failed" and calls (exit 1) when
;;   F > 0.  If the program ends without that line, the run failed.

(define-library (chibi test)
  (export test test-assert test-values test-error
          test-begin test-end test-group test-equal?)
  (import (scheme base)
          (scheme write)
          (scheme process-context))
  (begin

    (define test-epsilon 1e-5)

    ;; ------------------------------------------------------------
    ;; Comparison

    (define (test-nan? x)
      (not (= x x)))

    (define (test-approx=? a b)
      (cond ((= a b) #t)
            ((and (test-nan? a) (test-nan? b)) #t)
            ((or (test-nan? a) (test-nan? b)) #f)
            ((zero? a) (< (abs b) test-epsilon))
            (else (< (abs (/ (- a b) a)) test-epsilon))))

    (define (test-equal? expect res)
      (cond
       ((and (number? expect) (real? expect) (inexact? expect))
        (and (number? res) (real? res) (inexact? res)
             (test-approx=? expect res)))
       ((pair? expect)
        (and (pair? res)
             (test-equal? (car expect) (car res))
             (test-equal? (cdr expect) (cdr res))))
       ((vector? expect)
        (and (vector? res)
             (= (vector-length expect) (vector-length res))
             (let loop ((i 0))
               (or (= i (vector-length expect))
                   (and (test-equal? (vector-ref expect i)
                                     (vector-ref res i))
                        (loop (+ i 1)))))))
       (else (equal? expect res))))

    ;; ------------------------------------------------------------
    ;; Groups and counters
    ;;
    ;; A group is a vector #(name count passed failed).  test-groups is
    ;; the stack of open groups, innermost first.

    (define test-groups '())
    (define total-count 0)
    (define total-passed 0)
    (define total-failed 0)

    (define (group-name g) (vector-ref g 0))
    (define (group-inc! g i) (vector-set! g i (+ 1 (vector-ref g i))))

    (define (test-begin . o)
      (let ((name (if (pair? o) (car o) "")))
        (write-indent)
        (display "* ")
        (display name)
        (newline)
        (set! test-groups (cons (vector name 0 0 0) test-groups))))

    (define (test-end . o)
      (if (null? test-groups)
          (error "test-end without matching test-begin"))
      (let ((g (car test-groups)))
        (set! test-groups (cdr test-groups))
        (write-indent)
        (display "-- ")
        (display (group-name g))
        (display ": ")
        (display (vector-ref g 2))
        (display " of ")
        (display (vector-ref g 1))
        (display " passed")
        (if (> (vector-ref g 3) 0)
            (begin (display ", ")
                   (display (vector-ref g 3))
                   (display " FAILED")))
        (newline)
        (if (null? test-groups)
            (begin
              (display total-count)
              (display " tests, ")
              (display total-passed)
              (display " passed, ")
              (display total-failed)
              (display " failed")
              (newline)
              (flush-output-port)
              (if (> total-failed 0)
                  (exit 1))))))

    (define-syntax test-group
      (syntax-rules ()
        ((_ name body ...)
         (begin (test-begin name) body ... (test-end name)))))

    (define (write-indent)
      (let loop ((g test-groups))
        (if (pair? g)
            (begin (display "  ") (loop (cdr g))))))

    (define (record! pass?)
      (set! total-count (+ total-count 1))
      (if pass?
          (set! total-passed (+ total-passed 1))
          (set! total-failed (+ total-failed 1)))
      (for-each (lambda (g)
                  (group-inc! g 1)
                  (group-inc! g (if pass? 2 3)))
                test-groups))

    ;; ------------------------------------------------------------
    ;; Running one test

    ;; Run thunk; return (ok . value) or (raised . condition).
    (define (test-capture thunk)
      (guard (e (#t (cons 'raised e)))
        (cons 'ok (thunk))))

    (define (write-condition e)
      (cond ((error-object? e)
             (display "error: ")
             (write (error-object-message e))
             (for-each (lambda (x) (display " ") (write x))
                       (error-object-irritants e)))
            (else
             (display "raised non-error object: ")
             (write e))))

    (define (report-failure name source kind expect-desc expect res)
      (write-indent)
      (display kind)
      (display ": ")
      (if name
          (begin (display name) (display ": ")))
      (write source)
      (newline)
      (write-indent)
      (display "    expected ")
      (if expect-desc
          (display expect-desc)
          (write expect))
      (newline)
      (write-indent)
      (display "    but got ")
      (if (eq? (car res) 'raised)
          (write-condition (cdr res))
          (write (cdr res)))
      (newline))

    ;; mode is one of: value, assert, error
    (define (test-run name source mode expect-thunk expr-thunk)
      (case mode
        ((error)
         (let ((res (test-capture expr-thunk)))
           (if (eq? (car res) 'raised)
               (record! #t)
               (begin
                 (record! #f)
                 (report-failure name source "FAIL" "an error to be raised"
                                 #f res)))))
        ((assert)
         (let ((res (test-capture expr-thunk)))
           (cond ((and (eq? (car res) 'ok) (cdr res))
                  (record! #t))
                 (else
                  (record! #f)
                  (report-failure name source
                                  (if (eq? (car res) 'raised) "ERROR" "FAIL")
                                  "a true value" #f res)))))
        (else
         (let ((exp (test-capture expect-thunk)))
           (if (eq? (car exp) 'raised)
               (begin
                 (record! #f)
                 (report-failure name source "ERROR"
                                 "(the expected expression itself raised)"
                                 #f exp))
               (let ((res (test-capture expr-thunk)))
                 (cond ((and (eq? (car res) 'ok)
                             (test-equal? (cdr exp) (cdr res)))
                        (record! #t))
                       (else
                        (record! #f)
                        (report-failure name source
                                        (if (eq? (car res) 'raised)
                                            "ERROR" "FAIL")
                                        #f (cdr exp) res)))))))))

    (define-syntax test
      (syntax-rules ()
        ((_ expect expr)
         (test-run #f 'expr 'value (lambda () expect) (lambda () expr)))
        ((_ name expect expr)
         (test-run name 'expr 'value (lambda () expect) (lambda () expr)))))

    (define-syntax test-assert
      (syntax-rules ()
        ((_ expr)
         (test-run #f 'expr 'assert (lambda () #f) (lambda () expr)))
        ((_ name expr)
         (test-run name 'expr 'assert (lambda () #f) (lambda () expr)))))

    (define-syntax test-values
      (syntax-rules ()
        ((_ expect expr)
         (test-values #f expect expr))
        ((_ name expect expr)
         (test-run name 'expr 'value
                   (lambda ()
                     (call-with-values (lambda () expect) list))
                   (lambda ()
                     (call-with-values (lambda () expr) list))))))

    (define-syntax test-error
      (syntax-rules ()
        ((_ expr)
         (test-run #f 'expr 'error (lambda () #f) (lambda () expr)))
        ((_ name expr)
         (test-run name 'expr 'error (lambda () #f) (lambda () expr)))))))
