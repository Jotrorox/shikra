;; Spec §9.1 "Symbol table: string->symbol or read is reachable": string->symbol links the symbol
;; table, but no reader and no float printer (no flonum can reach a printer). A symbol literal
;; alone needs no table: see pay_float.scm.
(import (scheme base) (scheme write))
(write (string->symbol (string-append "sym" (number->string (string-length "abc")))))
(newline)
