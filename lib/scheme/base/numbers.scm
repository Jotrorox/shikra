;; Numbers (R7RS 6.2): fixnums and flonums (spec 3.2). The variadic procedures fold the
;; binary primitives; known calls pick their case-lambda clause at compile time.
(export number? complex? real? rational? integer? exact? inexact? exact-integer?
        = < > <= >= zero? positive? negative? odd? even? max min + * - / abs
        quotient remainder modulo floor-quotient floor-remainder floor/
        truncate-quotient truncate-remainder truncate/ gcd lcm
        floor ceiling round truncate exact inexact square exact-integer-sqrt expt
        numerator denominator rationalize number->string string->number)
(begin
  (define (number? x) (%number? x))
  (define (complex? x) (%number? x))
  (define (real? x) (%number? x))
  (define (rational? x) (if (%number? x) (%finite? x) #f))
  (define (integer? x) (%integer? x))
  (define (exact? x) (%exact? x))
  (define (inexact? x) (%inexact? x))
  (define (exact-integer? x) (%exact-integer? x))

  ;; Explicit clauses for up to four arguments, so a known call inlines to nested binary
  ;; primitives (every argument is still checked, in order, as by the general clause).
  (define-syntax define-comparison
    (syntax-rules ()
      ((_ name prim)
       (define name
         (case-lambda
           ((a b) (prim a b))
           ((a) (prim a a) #t)
           ((a b c) (let ((ok (prim a b))) (if (prim b c) ok #f)))
           ((a b c d) (let* ((ok (prim a b)) (ok (if (prim b c) ok #f))) (if (prim c d) ok #f)))
           ((a b . rest)
            (let loop ((x b) (rest rest) (ok (prim a b)))
              (if (%null? rest)
                  ok
                  (let ((y (%car rest)))
                    (loop y (%cdr rest) (if (prim x y) ok #f)))))))))))
  (define-comparison = %=)
  (define-comparison < %<)
  (define-comparison > %>)
  (define-comparison <= %<=)
  (define-comparison >= %>=)

  (define-syntax define-fold
    (syntax-rules ()
      ((_ name prim identity)
       (define name
         (case-lambda
           (() identity)
           ((a) (prim a identity))
           ((a b) (prim a b))
           ((a b c) (prim (prim a b) c))
           ((a b c d) (prim (prim (prim a b) c) d))
           ((a b . rest)
            (let loop ((acc (prim a b)) (rest rest))
              (if (%null? rest) acc (loop (prim acc (%car rest)) (%cdr rest))))))))))
  (define-fold + %+ 0)
  (define-fold * %* 1)

  (define-syntax define-inverse
    (syntax-rules ()
      ((_ name prim one)
       (define name
         (case-lambda
           ((a) (prim one a))
           ((a b) (prim a b))
           ((a b c) (prim (prim a b) c))
           ((a b . rest)
            (let loop ((acc (prim a b)) (rest rest))
              (if (%null? rest) acc (loop (prim acc (%car rest)) (%cdr rest))))))))))
  (define-inverse / %/ 1)
  (define -
    (case-lambda
      ((a) (%negate a))
      ((a b) (%- a b))
      ((a b c) (%- (%- a b) c))
      ((a b . rest)
       (let loop ((acc (%- a b)) (rest rest))
         (if (%null? rest) acc (loop (%- acc (%car rest)) (%cdr rest)))))))

  (define (zero? x) (%= x 0))
  (define (positive? x) (%> x 0))
  (define (negative? x) (%< x 0))
  (define (odd? n)
    (if (%integer? n) (if (%= (%remainder n 2) 0) #f #t) (type-error "odd?" "integer" n)))
  (define (even? n)
    (if (%integer? n) (%= (%remainder n 2) 0) (type-error "even?" "integer" n)))
  (define (extremum who a rest pick?)
    (let loop ((m a) (rest rest) (inexact #f))
      (if (%number? m) #f (type-error who "number" m))
      (if (%null? rest)
          (if (if inexact #t (%inexact? m)) (%inexact m) m)
          (let ((x (%car rest)))
            (if (%number? x) #f (type-error who "number" x))
            (loop (if (pick? x m) x m) (%cdr rest)
                  (if inexact #t (if (%inexact? x) #t (%inexact? m))))))))
  (define (max a . rest) (extremum "max" a rest (lambda (x m) (%> x m))))
  (define (min a . rest) (extremum "min" a rest (lambda (x m) (%< x m))))
  (define (abs x) (%abs x))

  (define (quotient a b) (%quotient a b))
  (define (remainder a b) (%remainder a b))
  (define (modulo a b) (%modulo a b))
  (define (floor-quotient a b) (%floor-quotient a b))
  (define (floor-remainder a b) (%modulo a b))
  (define (floor/ a b) (%values (%floor-quotient a b) (%modulo a b)))
  (define (truncate-quotient a b) (%quotient a b))
  (define (truncate-remainder a b) (%remainder a b))
  (define (truncate/ a b) (%values (%quotient a b) (%remainder a b)))
  (define (gcd2 a b) (if (%= b 0) (%abs a) (gcd2 b (%remainder a b))))
  (define (gcd . ns)
    (let loop ((acc 0) (ns ns))
      (if (%null? ns) acc (loop (gcd2 acc (%car ns)) (%cdr ns)))))
  (define (lcm . ns)
    (let loop ((acc 1) (ns ns))
      (if (%null? ns)
          acc
          (let ((n (%car ns)))
            (loop (if (if (%= acc 0) #t (%= n 0))
                      (%* acc 0)
                      (%abs (%* (%quotient acc (gcd2 acc n)) n)))
                  (%cdr ns))))))

  (define (floor x) (%floor x))
  (define (ceiling x) (%ceiling x))
  (define (round x) (%round x))
  (define (truncate x) (%truncate x))
  (define (exact x) (%exact x))
  (define (inexact x) (%inexact x))
  (define (square x) (%* x x)))
;; --bignum (feature bignum): Newton's iteration from 2^ceil(bits/2) in C, few divisions even for
;; large k; the default definition is unchanged (no bignum code in default programs).
(cond-expand
  (bignum
   (begin
     (define (exact-integer-sqrt k)
       (if (if (%exact-integer? k) (%< k 0) #t)
           (type-error "exact-integer-sqrt" "exact nonnegative integer" k))
       (let ((s (%exact-integer-sqrt k)))
         (%values s (%- k (%* s s)))))))
  (else
   (begin
  (define (exact-integer-sqrt k)
    (if (if (%exact-integer? k) (%< k 0) #t)
        (type-error "exact-integer-sqrt" "exact nonnegative integer" k))
    (if (%< k 2)
        (%values k 0)
        (let loop ((x (%quotient k 2)))
          (let ((y (%quotient (%+ x (%quotient k x)) 2)))
            (if (%>= y x)
                (%values x (%- k (%* x x)))
                (loop y)))))))))
(begin
  (define (expt z1 z2) (%expt z1 z2))
  ;; No exact rationals (spec 3.2): an exact integer is its own numerator; a finite flonum
  ;; q is n/d with d the smallest power of two that makes d*q integral (R7RS 6.2.6).
  (define (flonum-denominator who q)
    (if (if (%number? q) (%finite? q) #f)
        (let loop ((d 1.0)) (if (%integer? (%* d q)) d (loop (%* d 2.0))))
        (type-error who "rational number" q)))
  (define (numerator q)
    (if (%exact-integer? q) q (%* q (flonum-denominator "numerator" q))))
  (define (denominator q)
    (if (%exact-integer? q) 1 (flonum-denominator "denominator" q)))
  ;; The simplest rational within y of x (R7RS 6.2.6), computed in the arguments'
  ;; exactness: exact integers give an exact integer, otherwise a flonum.
  (define (rationalize x y)
    (let* ((y (%abs y)) (lo (%- x y)) (hi (%+ x y)))
      (cond ((if (%nan? lo) #t (%nan? hi)) (%+ lo hi))
            ((%> lo 0) (simplest-rational lo hi))
            ((%< hi 0) (%negate (simplest-rational (%negate hi) (%negate lo))))
            ((if (%exact? lo) (%exact? hi) #f) 0)
            (else 0.0))))
  (define (simplest-rational lo hi)     ; 0 < lo <= hi
    (let ((fl (%floor lo)))
      (cond ((%= fl lo) fl)
            ((%< fl (%floor hi)) (%+ fl 1))
            (else (%+ fl (%/ 1 (simplest-rational (%/ 1 (%- hi fl)) (%/ 1 (%- lo fl)))))))))

  (define number->string
    (case-lambda
      ((n) (%number->string n 10))
      ((n radix) (%number->string n radix))))
  (define string->number
    (case-lambda
      ((s) (%string->number s 10))
      ((s radix) (%string->number s radix)))))
