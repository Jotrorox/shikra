;; compile.ss -- bench/run.sh helper: chez --script compile.ss IN.scm OUT.so
(import (chezscheme))
(let ((args (cdr (command-line))))
  (compile-program (car args) (cadr args))
  (exit 0))
