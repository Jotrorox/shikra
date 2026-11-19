;; Numbers (R7RS 6.2): fixnums and flonums (spec 3.2). The variadic procedures fold the
;; binary primitives; known calls pick their case-lambda clause at compile time.
(export number? complex? real? rational? integer? exact? inexact? exact-integer?
        = < > <= >= zero? positive? negative? odd? even? max min + * - / abs
        quotient remainder modulo floor-quotient floor-remainder floor/
        truncate-quotient truncate-remainder truncate/ gcd lcm
        floor ceiling round truncate exact inexact square exact-integer-sqrt
        number->string string->number)
(begin
  (define (number? x) (%number? x))
  (define (complex? x) (%number? x))
  (define (real? x) (%number? x))
  (define (rational? x) (if (%number? x) (%finite? x) #f))
  (define (integer? x) (%integer? x))
  (define (exact? x) (%exact? x))
  (define (inexact? x) (%inexact? x))
  (define (exact-integer? x) (%exact-integer? x))

  (define-syntax define-comparison
    (syntax-rules ()
      ((_ name prim)
       (define name
         (case-lambda
           ((a b) (prim a b))
           ((a) (prim a a) #t)
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
           ((a b . rest)
            (let loop ((acc (prim a b)) (rest rest))
              (if (%null? rest) acc (loop (prim acc (%car rest)) (%cdr rest))))))))))
  (define-inverse / %/ 1)
  (define -
    (case-lambda
      ((a) (%* -1 a))
      ((a b) (%- a b))
      ((a b . rest)
       (let loop ((acc (%- a b)) (rest rest))
         (if (%null? rest) acc (loop (%- acc (%car rest)) (%cdr rest)))))))

  (define (zero? x) (%= x 0))
  (define (positive? x) (%> x 0))
  (define (negative? x) (%< x 0))
  (define (odd? n) (if (%= (%remainder n 2) 0) #f #t))
  (define (even? n) (%= (%remainder n 2) 0))
  (define (extremum a rest pick?)
    (let loop ((m a) (rest rest) (inexact (%inexact? a)))
      (if (%null? rest)
          (if inexact (%inexact m) m)
          (let ((x (%car rest)))
            (loop (if (pick? x m) x m) (%cdr rest) (if (%inexact? x) #t inexact))))))
  (define (max a . rest) (extremum a rest (lambda (x m) (%> x m))))
  (define (min a . rest) (extremum a rest (lambda (x m) (%< x m))))
  (define (abs x) (if (%< x 0) (%* -1 x) x))

  (define (quotient a b) (%quotient a b))
  (define (remainder a b) (%remainder a b))
  (define (modulo a b) (%modulo a b))
  (define (floor-quotient a b) (%floor-quotient a b))
  (define (floor-remainder a b) (%modulo a b))
  (define (floor/ a b) (%values (%floor-quotient a b) (%modulo a b)))
  (define (truncate-quotient a b) (%quotient a b))
  (define (truncate-remainder a b) (%remainder a b))
  (define (truncate/ a b) (%values (%quotient a b) (%remainder a b)))
  (define (gcd2 a b) (if (%= b 0) (abs a) (gcd2 b (%remainder a b))))
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
                      (abs (%quotient (%* acc n) (gcd2 acc n))))
                  (%cdr ns))))))

  (define (floor x) (%floor x))
  (define (ceiling x) (%ceiling x))
  (define (round x) (%round x))
  (define (truncate x) (%truncate x))
  (define (exact x) (%exact x))
  (define (inexact x) (%inexact x))
  (define (square x) (%* x x))
  (define (exact-integer-sqrt k)
    (if (if (%exact-integer? k) (%< k 0) #t)
        (type-error "exact-integer-sqrt" "exact nonnegative integer" k))
    (if (%< k 2)
        (%values k 0)
        (let loop ((x (%quotient k 2)))
          (let ((y (%quotient (%+ x (%quotient k x)) 2)))
            (if (%>= y x)
                (%values x (%- k (%* x x)))
                (loop y))))))

  (define number->string
    (case-lambda
      ((n) (%number->string n 10))
      ((n radix) (%number->string n radix))))
  (define string->number
    (case-lambda
      ((s) (%string->number s 10))
      ((s radix) (%string->number s radix)))))
