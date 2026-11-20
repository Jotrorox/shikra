;; bank.scm: accounts as records, errors as exceptions.
;;
;;   shikra run bank.scm
;;
;; Shows: define-record-type, raising your own condition objects and R7RS error objects,
;; guard with => and else clauses, dynamic-wind for clean-up, and parameterize.
(import (scheme base) (scheme write))

(define-record-type account
  (make-account owner balance)
  account?
  (owner account-owner)
  (balance account-balance set-account-balance!))

;; A condition of our own, distinct from error objects.
(define-record-type insufficient-funds
  (make-insufficient-funds account amount)
  insufficient-funds?
  (account insufficient-account)
  (amount insufficient-amount))

;; The currency symbol is a parameter: callers can rebind it for a dynamic extent.
(define currency (make-parameter "EUR"))

(define (show-money cents)
  (string-append (number->string (quotient cents 100)) "."
                 (let ((c (remainder cents 100)))
                   (if (< c 10) (string-append "0" (number->string c)) (number->string c)))
                 " " (currency)))

(define (withdraw! acct cents)
  (unless (and (exact-integer? cents) (positive? cents))
    (error "withdraw!: amount must be a positive number of cents" cents))
  (when (> cents (account-balance acct))
    (raise (make-insufficient-funds acct cents)))
  (set-account-balance! acct (- (account-balance acct) cents)))

(define (deposit! acct cents)
  (set-account-balance! acct (+ (account-balance acct) cents)))

;; Moves money between accounts, or neither balance changes.
(define (transfer! from to cents)
  (withdraw! from cents)
  (deposit! to cents))

(define (report . accounts)
  (for-each (lambda (a)
              (display "  ") (display (account-owner a)) (display ": ")
              (display (show-money (account-balance a))) (newline))
            accounts))

(define alice (make-account "alice" 10000))
(define bob (make-account "bob" 2550))

;; Runs a transaction, logs how it ended, and returns 'ok or the reason it failed.
(define (attempt description thunk)
  (display description) (newline)
  (guard (e ((insufficient-funds? e)
             (display "  refused: ") (display (account-owner (insufficient-account e)))
             (display " cannot pay ") (display (show-money (insufficient-amount e)))
             (newline)
             'refused)
            ((error-object? e)
             (display "  error: ") (display (error-object-message e))
             (display " ") (write (error-object-irritants e)) (newline)
             'error))
    (dynamic-wind
     (lambda () #f)
     thunk
     (lambda () (report alice bob)))   ; runs however the transaction ends
    'ok))

(write (attempt "alice pays bob 25.00" (lambda () (transfer! alice bob 2500))))
(newline)
(write (attempt "bob pays alice 100.00" (lambda () (transfer! bob alice 10000))))
(newline)
(write (attempt "alice pays bob -5" (lambda () (transfer! alice bob -5))))
(newline)
(parameterize ((currency "CHF"))
  (write (attempt "in francs: bob pays alice 0.07" (lambda () (transfer! bob alice 7))))
  (newline))
