(import (scheme base) (scheme eval))
(display (eval (quote (+ 1 2)) (environment (quote (scheme base)))))
