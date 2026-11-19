;; --bignum: (shikra fixnum) stays fixnum-only -- fixnum? tells bignums apart, fx+ fx- fx* fxneg still
;; raise on overflow -- while the generic operations promote. Expected output:
;; tests/cases/inc/bignum_ref.py.
(import (scheme base) (scheme write) (shikra fixnum))
(define (show . xs) (for-each (lambda (x) (write x) (display " ")) xs) (newline))
(define-syntax try
  (syntax-rules () ((_ e) (guard (x ((error-object? x) 'overflow)) e))))
(show 'fixnum? (fixnum? fx-greatest) (fixnum? (+ fx-greatest 1)) (fixnum? (- fx-least 1)) (fixnum? (- (+ fx-greatest 1) 1))
      (fixnum? (quotient (* fx-greatest 4) 4)))
(show 'fx+ (try (fx+ fx-greatest 1)) (try (fx- fx-least 1)) (try (fx* fx-greatest 2)) (try (fxneg fx-least)))
(show 'generic (+ fx-greatest 1) (- fx-least 1) (* fx-greatest 2) (- fx-least))
