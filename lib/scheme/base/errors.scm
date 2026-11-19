;; Errors. M0 has no handler stack: raise and error report the uncaught object and exit
;; (status 70). TODO(M1): with-exception-handler, raise-continuable, guard, and the
;; Scheme-level raise that consults the handler stack.
(export raise error error-object? error-object-message error-object-irritants
        file-error? read-error?)
(begin
  (define (raise obj) (%raise obj))
  (define (error message . irritants)
    (%raise (%make-error-object 0 message irritants)))
  ;; library-internal: "who: expected what" with the offending object as irritant
  (define (type-error who what obj)
    (%raise (%make-error-object 0 (%string-append who (%string-append ": expected " what))
                                (%cons obj '()))))
  (define (error-object? x) (%error-object? x))
  (define (error-object-message e) (%error-object-message e))
  (define (error-object-irritants e) (%error-object-irritants e))
  (define (file-error? x) (%file-error? x))
  (define (read-error? x) (%read-error? x)))
