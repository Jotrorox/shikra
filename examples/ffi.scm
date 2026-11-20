;; ffi.scm: calling C.
;;
;;   shikra run --link ffi.c ffi.scm      # flags before the file: what follows it goes to
;;   shikra build --link ffi.c ffi.scm -o ffi    # the program as its arguments
;;
;; (define-c name "c_function" (argument types) result type) binds a C function. The C
;; library's functions need nothing more; your own C files are compiled and linked with
;; --link (also .o and .a files) and libraries with -l NAME. Types: int long double float
;; bool char string pointer void. Shikra checks the Scheme arguments, converts them, and
;; calls the function directly: no wrapper, no marshalling buffer.
(import (scheme base) (scheme write) (shikra ffi))

;; From the C library.
(define-c c-strlen "strlen" (string) long)
(define-c c-hypot "hypot" (double double) double)
(define-c c-abs "labs" (long) long)

;; From ffi.c.
(define-c stats-reset! "stats_reset" () void)
(define-c stats-add! "stats_add" (double) bool)
(define-c stats-stddev "stats_stddev" () double)
(define-c shout "shout" (string) string)

(write (list (c-strlen "hello") (c-hypot 3.0 4.0) (c-abs -42)))
(newline)

(stats-reset!)
(for-each stats-add! '(2.0 4.0 4.0 4.0 5.0 5.0 7.0 9.0))
(display "standard deviation: ") (write (stats-stddev)) (newline)

(write (shout "hello from scheme"))
(newline)
