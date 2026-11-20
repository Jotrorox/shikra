;; Spec §9.1 "Unicode tables: --unicode=full is set" and §3.1 "(scheme char): ASCII tables unless
;; --unicode=full": the same program as pay_ascii.scm, with the full tables.
(import (scheme base) (scheme char) (scheme write))
(write (list (char-upcase (string-ref "\xe4;" 0)) (char-alphabetic? (string-ref "\x3bb;" 0))))
(newline)
