;; Upstream src/GambitC-prelude.scm plus (not safe), for bench/run.sh gambit-unsafe.
(declare (standard-bindings)   ;; builtin functions like + will not be redefined
         (extended-bindings)   ;; Gambit functions like fixnum? will not be redefined
         (block)               ;; user-defined functions not set! in the file will not be redefined
         (not safe)            ;; unsafe: no type checks
         )

(define (this-scheme-implementation-name) (string-append "gambitc-" (system-version-string)))
