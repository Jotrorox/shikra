;; Input and output (R7RS 6.13): textual and binary ports over OS descriptors, strings and
;; bytevectors (rt_io.c). A port is textual or binary, never both; the standard ports are
;; textual. The current ports are parameter objects (control.scm). display and write are in
;; (scheme write); file ports are (scheme file).
(export current-input-port current-output-port current-error-port call-with-port
        port? input-port? output-port? textual-port? binary-port?
        input-port-open? output-port-open? close-port close-input-port close-output-port
        open-input-string open-output-string get-output-string
        open-input-bytevector open-output-bytevector get-output-bytevector
        read-char peek-char read-line read-string char-ready?
        read-u8 peek-u8 u8-ready? read-bytevector read-bytevector!
        eof-object eof-object?
        write-char write-string newline write-u8 write-bytevector flush-output-port)
(begin
  (define current-input-port (make-parameter (%stdin-port)))
  (define current-output-port (make-parameter (%stdout-port)))
  (define current-error-port (make-parameter (%stderr-port)))
  (define (call-with-port port proc)
    (call-with-values (lambda () (proc port))
      (lambda vals (%close-port port) (%apply values vals))))
  (define (port? x) (%port? x))
  (define (input-port? x) (%input-port? x))
  (define (output-port? x) (%output-port? x))
  (define (textual-port? x) (%textual-port? x))
  (define (binary-port? x) (%binary-port? x))
  (define (input-port-open? p) (%input-port-open? p))
  (define (output-port-open? p) (%output-port-open? p))
  (define (close-port p) (%close-port p))
  (define (close-input-port p)
    (if (%input-port? p) (%close-port p) (type-error "close-input-port" "input port" p)))
  (define (close-output-port p)
    (if (%output-port? p) (%close-port p) (type-error "close-output-port" "output port" p)))
  (define (open-input-string s) (%open-input-string s))
  (define (open-output-string) (%open-output-string))
  (define (get-output-string p) (%get-output-string p))
  (define (open-input-bytevector bv) (%open-input-bytevector bv))
  (define (open-output-bytevector) (%open-output-bytevector))
  (define (get-output-bytevector p) (%get-output-bytevector p))

  (define read-char
    (case-lambda (() (%read-char (current-input-port))) ((p) (%read-char p))))
  (define peek-char
    (case-lambda (() (%peek-char (current-input-port))) ((p) (%peek-char p))))
  (define read-line
    (case-lambda (() (%read-line (current-input-port))) ((p) (%read-line p))))
  (define read-string
    (case-lambda ((k) (%read-string k (current-input-port))) ((k p) (%read-string k p))))
  (define char-ready?
    (case-lambda (() (%char-ready? (current-input-port))) ((p) (%char-ready? p))))
  (define read-u8
    (case-lambda (() (%read-u8 (current-input-port))) ((p) (%read-u8 p))))
  (define peek-u8
    (case-lambda (() (%peek-u8 (current-input-port))) ((p) (%peek-u8 p))))
  (define u8-ready?
    (case-lambda (() (%u8-ready? (current-input-port))) ((p) (%u8-ready? p))))
  (define read-bytevector
    (case-lambda
      ((k) (read-bytevector k (current-input-port)))
      ((k p)
       (let* ((bv (%make-bytevector k 0))
              (n (%read-bytevector! bv p 0 k)))
         (cond ((%eof-object? n) n)
               ((%= n k) bv)
               (else (%bytevector-copy bv 0 n)))))))
  (define read-bytevector!
    (case-lambda
      ((bv) (%read-bytevector! bv (current-input-port) 0 (%bytevector-length bv)))
      ((bv p) (%read-bytevector! bv p 0 (%bytevector-length bv)))
      ((bv p start) (%read-bytevector! bv p start (%bytevector-length bv)))
      ((bv p start end) (%read-bytevector! bv p start end))))
  (define (eof-object) (%eof-object))
  (define (eof-object? x) (%eof-object? x))

  (define write-char
    (case-lambda ((c) (%write-char c (current-output-port))) ((c p) (%write-char c p))))
  (define write-string
    (case-lambda
      ((s) (%write-string s (current-output-port)))
      ((s p) (%write-string s p))
      ((s p start) (%write-string (%substring s start (%string-length s)) p))
      ((s p start end) (%write-string (%substring s start end) p))))
  (define newline
    (case-lambda (() (%newline (current-output-port))) ((p) (%newline p))))
  (define write-u8
    (case-lambda ((b) (%write-u8 b (current-output-port))) ((b p) (%write-u8 b p))))
  (define write-bytevector
    (case-lambda
      ((bv) (%write-bytevector bv (current-output-port) 0 (%bytevector-length bv)))
      ((bv p) (%write-bytevector bv p 0 (%bytevector-length bv)))
      ((bv p start) (%write-bytevector bv p start (%bytevector-length bv)))
      ((bv p start end) (%write-bytevector bv p start end))))
  (define flush-output-port
    (case-lambda (() (%flush-output-port (current-output-port))) ((p) (%flush-output-port p)))))
