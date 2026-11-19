;; Chez postlude for the r7rs-benchmarks (bench/run.sh); same name as upstream's
;; src/Chez-prelude.scm.
(define (this-scheme-implementation-name)
  (string-append "chez-"
                 (call-with-values scheme-version-number
                   (lambda (a b c)
                     (string-append (number->string a) "." (number->string b)
                                    "." (number->string c))))
                 "+r7rs"))
