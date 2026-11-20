;; Spec §9.1 "File ports: (scheme file) procedures are reachable": opening a file links the file
;; port code; pay_symbols.scm, which has no (scheme file), does not (checked there).
(import (scheme base) (scheme write) (scheme file))
(write (call-with-input-file "tests/shape/pay_file.scm" read-line))
(newline)
