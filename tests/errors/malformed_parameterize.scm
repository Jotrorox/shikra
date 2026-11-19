(import (scheme base))
(define p (make-parameter 1))
(parameterize ((p)) 1)
