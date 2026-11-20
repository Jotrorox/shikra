;; Spec §9.1 "Unicode tables: --unicode=full is set" and §3.1 "(scheme char): ASCII tables unless
;; --unicode=full": without the flag no table is linked and non-ASCII characters are not letters.
(import (scheme base) (scheme char) (scheme write))
(write (list (char-upcase (string-ref "\xe4;" 0)) (char-alphabetic? (string-ref "\x3bb;" 0))))
(newline)
