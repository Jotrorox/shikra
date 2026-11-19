;; read-line / read-string / peek-char / read-char on stdin (ports_stdin.in), whose lines and
;; chunks cross the 4 KB port buffer: a 2-byte char straddles byte 4096, a 5000-char line of
;; 3-byte chars, a CRLF line, an empty line, a block with 4-byte chars, no final newline.
;; Each read prints its length, a checksum and its last char. Then the same reads from a
;; string port over a large string, checked against substring.
(import (scheme base) (scheme write))

(define (checksum s)
  (let ((acc 0))
    (string-for-each (lambda (c) (set! acc (modulo (+ (* acc 31) (char->integer c)) 1000003))) s)
    acc))
(define (show tag s)
  (display tag)
  (if (eof-object? s)
      (display " eof")
      (begin
        (display " ") (display (string-length s))
        (display " ") (display (checksum s))
        (if (> (string-length s) 0)
            (begin (display " ") (display (string-ref s (- (string-length s) 1)))))))
  (newline))

(show "line" (read-line)) (show "line" (read-line)) (show "line" (read-line)) (show "line" (read-line))
(show "str" (read-string 3000))
(show "str" (read-string 3000 (current-input-port)))
(display "peek ") (display (peek-char)) (newline)
(show "str" (read-string 100000))
(show "line" (read-line))
(display (if (eof-object? (read-char)) "char eof" "char")) (newline)
(show "str" (read-string 5))
(display (guard (e ((error-object? e) (error-object-message e))) (read-u8))) (newline)

;; string port: 3000 lines "<n>:λ€😀..." of growing length, read back two ways
(define (make-line i) (string-append (number->string i) ":" (make-string (modulo i 50) #\λ) "€😀"))
(define big
  (let ((out (open-output-string)))
    (do ((i 0 (+ i 1))) ((= i 3000)) (write-string (make-line i) out) (newline out))
    (get-output-string out)))
(define in (open-input-string big))
(define lines-ok
  (let loop ((i 0) (ok #t))
    (let ((l (read-line in)))
      (if (eof-object? l)
          (and ok (= i 3000))
          (loop (+ i 1) (and ok (string=? l (make-line i))))))))
(display (list 'lines lines-ok (string-length big))) (newline)
(define in2 (open-input-string big))
(define chunks-ok
  (let loop ((pos 0) (ok #t))
    (let ((s (read-string 777 in2)))
      (if (eof-object? s)
          (and ok (= pos (string-length big)))
          (loop (+ pos (string-length s))
                (and ok (string=? s (substring big pos (+ pos (string-length s))))))))))
(display (list 'chunks chunks-ok)) (newline)
