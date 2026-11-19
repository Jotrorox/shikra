;; string->number (R7RS 6.2.7): #f, never an error, for anything that is not a number Shikra
;; can represent (spec 3.2): malformed text, exact integers beyond the fixnum range,
;; exact ratios whose parts are beyond it, complex numbers, #e of a non-integer.
(import (scheme base) (util check))

(define (all-false . strs) (map (lambda (s) (string->number s)) strs))
(check (all-false "" "+" "-" "." "..1" "e1" "1e" "1e+" "abc" "1 2" " 1" "1 " "#" "#x" "#xg"
                  "#b2" "#o8" "1/" "/2" "1/2/3" "1.5/2" "1/2.5" "--1" "+-1" "1+" "inf.0" "nan.0"
                  "+inf" "#e#e1" "#x#x1" "#e#i1" "#q1" "1#" "1_000")
      (make-list 34 #f))
;; beyond the fixnum range
(check (all-false "4611686018427387904" "-4611686018427387905" "99999999999999999999"
                  "#x4000000000000000" "#b1000000000000000000000000000000000000000000000000000000000000000"
                  "#x4000000000000001/3e1" "9223372036854775808/2" "1/4611686018427387904"
                  "#e1e19" "#e4611686018427387904.0")
       (make-list 10 #f))
;; no rationals, no complex numbers
(check (all-false "1/0" "#e1.5" "#e1/2" "1+2i" "+i" "-2.5i" "1@2" "#e+inf.0" "#e+nan.0")
       (make-list 9 #f))
;; still numbers
(check (string->number "4611686018427387903") 4611686018427387903)
(check (string->number "-4611686018427387904") -4611686018427387904)
(check (string->number "#x3fffffffffffffff") 4611686018427387903)
(check (string->number "#x-4000000000000000") -4611686018427387904)
(check (string->number "#i4611686018427387904") 4611686018427387904.0)
(check (string->number "99999999999999999999.0") 1e20)
(check (string->number "6/3") 2)
(check (string->number "-6/4") -1.5)
(check (string->number "#i1/4") 0.25)
(check (string->number "#e1e3") 1000)
(check (string->number "#e1.0") 1)
(check (string->number "#x10" 10) 16)
(check (string->number "10" 16) 16)
(check (string->number "#d10" 16) 10)
(check (string->number "-ff" 16) -255)
(check (string->number "1e2" 16) 482)
(check (string->number "101" 2) 5)
(check (string->number "777" 8) 511)
(check (string->number "1.5e2") 150.0)
(check (string->number ".5") 0.5)
(check (string->number "1.") 1.0)
(check (string->number "-.5e-1") -0.05)
(check (string->number "+inf.0") +inf.0)
(check (string->number "-INF.0") -inf.0)
(check (string->number "#i+inf.0") +inf.0)
(check (eqv? (string->number "-0.0") -0.0) #t)
(check (string->number "1.5" 16) #f)
(check-error "string->number" (string->number 'a))
(check-error "string->number" (string->number "1" 3))
(check-done)
