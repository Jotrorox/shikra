(import (scheme base) (shikra ffi))
(define-c c-f "f-g" (int) int)
(c-f 1)
