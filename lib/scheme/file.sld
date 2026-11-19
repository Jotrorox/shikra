;; (scheme file) -- R7RS 6.13.1, 6.14. File ports are OS-descriptor ports (rt_io.c), textual or
;; binary; every failure to open or delete raises an error satisfying file-error?. The
;; call-with-* and with-* procedures close the port when the procedure returns (not when it
;; escapes); with-input-from-file / with-output-to-file rebind the current port with
;; parameterize, so an escape restores the previous one.
(define-library (scheme file)
  (import (shikra primitives) (scheme base))
  (export call-with-input-file call-with-output-file delete-file file-exists?
          open-binary-input-file open-binary-output-file open-input-file open-output-file
          with-input-from-file with-output-to-file)
  (begin
    (define (open-input-file f) (%open-input-file f))
    (define (open-output-file f) (%open-output-file f))
    (define (open-binary-input-file f) (%open-binary-input-file f))
    (define (open-binary-output-file f) (%open-binary-output-file f))
    (define (file-exists? f) (%file-exists? f))
    (define (delete-file f) (%delete-file f))
    (define (call-with-input-file f proc) (call-with-port (open-input-file f) proc))
    (define (call-with-output-file f proc) (call-with-port (open-output-file f) proc))
    (define (with-input-from-file f thunk)
      (let ((p (open-input-file f)))
        (call-with-values (lambda () (parameterize ((current-input-port p)) (thunk)))
          (lambda vals (%close-port p) (apply values vals)))))
    (define (with-output-to-file f thunk)
      (let ((p (open-output-file f)))
        (call-with-values (lambda () (parameterize ((current-output-port p)) (thunk)))
          (lambda vals (%close-port p) (apply values vals)))))))
