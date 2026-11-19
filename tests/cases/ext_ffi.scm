;; (shikra ffi): the spec's two bindings verbatim, C library functions with each type, first-class
;; use, type and range errors naming the Scheme procedure. Runs in both engines (the interpreter
;; finds these functions with dlsym in its own process).
(import (scheme base) (scheme write) (shikra ffi) (util check))

(define-c c-hypot "hypot" (double double) double)
(define-c c-getpid "getpid" () int)

(define-c c-strlen "strlen" (string) long)
(define-c c-abs "abs" (int) int)
(define-c c-labs "labs" (long) long)
(define-c c-atoi "atoi" (string) int)
(define-c c-getenv "getenv" (string) string)
(define-c c-toupper "toupper" (int) int)
(define-c c-isdigit "isdigit" (char) int)
(define-c c-ldexp "ldexp" (double int) double)
(define-c c-fmin "fmin" (double double) double)
(define-c c-strchr "strchr" (string int) pointer)
(define-c c-strtol "strtol" (string pointer int) long)
(define-c c-free "free" (pointer) void)
(define-c c-malloc "malloc" (long) pointer)
(define-c c-puts "puts" (string) int)

(check (c-hypot 3.0 4.0) 5.0)
(check (c-hypot 5 12) 13.0) ; exact integers convert to double
(check (> (c-getpid) 0) #t)
(check (= (c-getpid) (c-getpid)) #t)
(check (list (c-strlen "") (c-strlen "hello") (c-strlen "λ")) '(0 5 2)) ; UTF-8 bytes
(check (list (c-abs -7) (c-abs 7) (c-labs -123456789012)) '(7 7 123456789012))
(check (list (c-atoi "42") (c-atoi "-17 apples") (c-atoi "x")) '(42 -17 0))
(check (c-getenv "SHIKRA_SURELY_UNSET_VARIABLE") #f)
(check (string? (c-getenv "PATH")) #t)
(check (integer->char (c-toupper (char->integer #\q))) #\Q)
(check (list (c-isdigit #\7) (c-isdigit #\x)) (list (c-isdigit #\7) 0))
(check (not (= 0 (c-isdigit #\7))) #t)
(check (c-ldexp 0.75 4) 12.0)
(check (c-fmin 2.0 -1.5) -1.5)
(check (c-strchr "abc" (char->integer #\z)) 0) ; NULL is 0
(check (> (c-strchr "abc" (char->integer #\b)) 0) #t)
(check (c-strtol "ff" 0 16) 255)
(let ((p (c-malloc 64))) (check (> p 0) #t) (c-free p))
(check (map c-abs '(-1 -2 3)) '(1 2 3)) ; first-class
(check (procedure? c-hypot) #t)
(check (apply c-hypot '(6.0 8.0)) 10.0)
(check-error "c-abs" (c-abs 2147483648))
(check-error "c-abs" (c-abs 1.5))
(check-error "c-strlen" (c-strlen 'hello))
(check-error "c-hypot" (c-hypot 1.0 "2"))
(check-error "c-isdigit" (c-isdigit 55))
(check-error "c-isdigit" (c-isdigit #\λ))
(check-error "c-free" (c-free -1))
(flush-output-port)
(check (>= (c-puts "from C") 0) #t)
(check-done)
