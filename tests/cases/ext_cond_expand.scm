;; Portable code guards the (shikra ...) imports with cond-expand on the shikra feature (spec §10):
;; the same program runs on any R7RS system, with the extensions where they exist.
(import (scheme base) (scheme write))
(cond-expand
  (shikra (import (shikra fixnum) (shikra flonum)))
  (else (begin (define fx+ +) (define fx<? <) (define fl+ +) (define fl* *))))
(cond-expand
  ((library (shikra f64vector)) (import (shikra f64vector)))
  (else (begin (define make-f64vector make-vector) (define f64vector-ref vector-ref) (define f64vector-set! vector-set!))))
(define (sum n) (let loop ((i 0) (s 0)) (if (fx<? i n) (loop (fx+ i 1) (fx+ s i)) s)))
(define v (make-f64vector 4 0.5))
(f64vector-set! v 0 2.0)
(write (list (sum 10) (fl+ (f64vector-ref v 0) (fl* (f64vector-ref v 1) 3.0))
             (cond-expand ((library (shikra assume)) 'assume) (else 'no-assume))
             (cond-expand ((library (shikra ffi)) 'ffi) (else 'no-ffi))
             (cond-expand ((library (shikra nothing)) 'nothing) (else 'no-nothing))
             (cond-expand ((and shikra (library (shikra flonum))) 'both) (else 'neither))))
(newline)
