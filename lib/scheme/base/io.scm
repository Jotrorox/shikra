;; Input and output (R7RS 6.13), textual ports. The current ports are plain procedures
;; until parameter objects exist (TODO(M1): make them parameters for parameterize).
;; display and write are in (scheme write).
(export current-input-port current-output-port current-error-port
        port? input-port? output-port? textual-port? close-port close-input-port
        close-output-port open-input-string open-output-string get-output-string
        read-char peek-char read-line read-string eof-object eof-object?
        write-char write-string newline flush-output-port)
(begin
  (define (current-input-port) (%stdin-port))
  (define (current-output-port) (%stdout-port))
  (define (current-error-port) (%stderr-port))
  (define (port? x) (%port? x))
  (define (input-port? x) (%input-port? x))
  (define (output-port? x) (%output-port? x))
  (define (textual-port? x) (%port? x))
  (define (close-port p) (%close-port p))
  (define (close-input-port p) (%close-port p))
  (define (close-output-port p) (%close-port p))
  (define (open-input-string s) (%open-input-string s))
  (define (open-output-string) (%open-output-string))
  (define (get-output-string p) (%get-output-string p))
  (define read-char
    (case-lambda (() (%read-char (%stdin-port))) ((p) (%read-char p))))
  (define peek-char
    (case-lambda (() (%peek-char (%stdin-port))) ((p) (%peek-char p))))
  (define read-line
    (case-lambda (() (%read-line (%stdin-port))) ((p) (%read-line p))))
  (define read-string
    (case-lambda
      ((k) (read-string k (%stdin-port)))
      ((k p)
       (let loop ((i 0) (acc '()))
         (if (%< i k)
             (let ((c (%read-char p)))
               (if (%eof-object? c)
                   (if (%null? acc) c (%list->string (%reverse acc)))
                   (loop (%+ i 1) (%cons c acc))))
             (%list->string (%reverse acc)))))))
  (define (eof-object) (%eof-object))
  (define (eof-object? x) (%eof-object? x))
  (define write-char
    (case-lambda ((c) (%write-char c (%stdout-port))) ((c p) (%write-char c p))))
  (define write-string
    (case-lambda
      ((s) (%write-string s (%stdout-port)))
      ((s p) (%write-string s p))
      ((s p start) (%write-string (%substring s start (%string-length s)) p))
      ((s p start end) (%write-string (%substring s start end) p))))
  (define newline
    (case-lambda (() (%newline (%stdout-port))) ((p) (%newline p))))
  (define flush-output-port
    (case-lambda (() (%flush-output-port (%stdout-port))) ((p) (%flush-output-port p)))))
