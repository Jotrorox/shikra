;; Spec §9.1 "Datum reader: read is reachable" (and with it the symbol table and float parsing):
;; the reader is linked here; pay_symbols.scm and pay_float.scm do not link it.
(import (scheme base) (scheme read) (scheme write))
(write (read (open-input-string "(a 1.5 \"s\")")))
(newline)
