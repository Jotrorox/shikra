;; (shikra ffi) -- define-c (spec §10): (define-c name "c-name" (arg-type ...) result-type)
;; binds name to a procedure that calls the C function. Types: int long double float bool char
;; string pointer, and void as a result. Compiled programs call the C function directly (the
;; compiler emits its prototype); the interpreter finds it with dlsym in the running process.
;; DESIGN.md "Extensions as built" has the conversion rules and the interpreter's shapes.
(define-library (shikra ffi)
  (import (shikra primitives))
  (export define-c))
