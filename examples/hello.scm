;; hello.scm: the smallest program.
;;
;;   shikra run hello.scm
;;   shikra build hello.scm -o hello && ./hello
;;
;; `shikra size hello` shows what is in the 50 KB: almost all of it is the executable format's
;; floor, not Scheme.
(import (scheme base) (scheme write))
(display "hello, world")
(newline)
