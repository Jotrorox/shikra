;; (scheme file) for Chez Scheme 10 -- see base.sls.
(library (scheme file)
  (export call-with-input-file call-with-output-file delete-file file-exists?
          open-binary-input-file open-binary-output-file open-input-file
          open-output-file with-input-from-file with-output-to-file)
  (import (chezscheme))
  (define (open-binary-input-file name) (open-file-input-port name))
  (define (open-binary-output-file name)
    (open-file-output-port name (file-options no-fail))))
