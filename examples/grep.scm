;; grep.scm: print the lines of files that contain a string, like a small `grep -n`.
;;
;;   shikra build grep.scm -o sgrep
;;   ./sgrep define *.scm            # name:line:text for every match
;;   ./sgrep -c define *.scm         # count matching lines per file
;;
;; Exit status as grep's: 0 if a line matched, 1 if none did, 2 on a usage error or a file
;; that cannot be read. Shows: (scheme file), (scheme process-context), guard on file errors,
;; writing to the error port.
(import (scheme base) (scheme file) (scheme write) (scheme process-context))

(define (usage)
  (write-string "usage: sgrep [-c] STRING FILE...\n" (current-error-port))
  (exit 2))

;; Index of the first occurrence of needle in haystack, or #f.
(define (string-search needle haystack)
  (let ((n (string-length needle))
        (h (string-length haystack)))
    (let loop ((i 0))
      (cond ((> (+ i n) h) #f)
            ((string=? needle (substring haystack i (+ i n))) i)
            (else (loop (+ i 1)))))))

;; Calls (found file line-number line) for each matching line; returns the number of
;; matches, or #f if the file cannot be opened.
(define (search-file needle file found)
  (guard (e ((file-error? e)
             (write-string (string-append "sgrep: cannot read " file "\n")
                           (current-error-port))
             #f))
    (call-with-input-file file
      (lambda (port)
        (let loop ((number 1) (matches 0))
          (let ((line (read-line port)))
            (cond ((eof-object? line) matches)
                  ((string-search needle line)
                   (found file number line)
                   (loop (+ number 1) (+ matches 1)))
                  (else (loop (+ number 1) matches)))))))))

(define (main args)
  (let* ((count-only (and (pair? args) (string=? (car args) "-c")))
         (args (if count-only (cdr args) args)))
    (when (or (null? args) (null? (cdr args))) (usage))
    (let loop ((files (cdr args)) (any-match #f) (any-error #f))
      (if (null? files)
          (exit (cond (any-error 2) (any-match 0) (else 1)))
          (let ((n (search-file (car args) (car files)
                                (lambda (file number line)
                                  (unless count-only
                                    (display file) (display ":")
                                    (display number) (display ":")
                                    (display line) (newline))))))
            (when (and n count-only)
              (display (car files)) (display ":") (display n) (newline))
            (loop (cdr files)
                  (or any-match (and n (> n 0)))
                  (or any-error (not n))))))))

(main (cdr (command-line)))
