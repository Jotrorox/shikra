;; Spec §9.1 "Float printing and parsing: a flonum can reach a printer": linked here, without the
;; symbol table (a quoted symbol is static, spec §6.1) and without the reader.
(import (scheme base) (scheme write))
(write (list 'ratio (/ (string-length "abcd") 3)))
(newline)
