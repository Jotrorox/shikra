;; wordfreq.scm: the most frequent words of the text on standard input.
;;
;;   shikra run wordfreq.scm 10 < some-text.txt
;;   shikra build wordfreq.scm -o wordfreq && ./wordfreq 10 < some-text.txt
;;
;; Shows: reading lines, (scheme char), string ports, a small hash table built on vectors
;; (R7RS-small has none), a merge sort on vectors, and command-line arguments.
(import (scheme base) (scheme char) (scheme write) (scheme process-context))

;; How many words to print: the first argument, default 10.
(define top
  (let ((args (cdr (command-line))))
    (or (and (pair? args) (string->number (car args))) 10)))

;; A hash table of string -> count: a vector of association-list buckets that doubles when
;; it gets full.
(define buckets (make-vector 64 '()))
(define entries 0)

(define (string-hash s size)
  (let loop ((i 0) (h 5381))
    (if (= i (string-length s))
        (modulo h size)
        (loop (+ i 1)
              (modulo (+ (* h 33) (char->integer (string-ref s i))) 16777213)))))

(define (grow!)
  (let ((old buckets))
    (set! buckets (make-vector (* 2 (vector-length old)) '()))
    (vector-for-each
     (lambda (bucket)
       (for-each (lambda (entry)
                   (let ((i (string-hash (car entry) (vector-length buckets))))
                     (vector-set! buckets i (cons entry (vector-ref buckets i)))))
                 bucket))
     old)))

(define (count! word)
  (let* ((i (string-hash word (vector-length buckets)))
         (entry (assoc word (vector-ref buckets i))))
    (if entry
        (set-cdr! entry (+ (cdr entry) 1))
        (begin
          (vector-set! buckets i (cons (cons word 1) (vector-ref buckets i)))
          (set! entries (+ entries 1))
          (when (> entries (vector-length buckets)) (grow!))))))

;; Split a line into lower-case words: runs of letters and apostrophes.
(define (for-each-word proc line)
  (let ((out (open-output-string)))
    (define (flush!)
      (let ((word (get-output-string out)))
        (unless (string=? word "")
          (proc word)
          (set! out (open-output-string)))))
    (string-for-each
     (lambda (c)
       (if (or (char-alphabetic? c) (char=? c #\'))
           (write-char (char-downcase c) out)
           (flush!)))
     line)
    (flush!)))

(let loop ()
  (let ((line (read-line)))
    (unless (eof-object? line)
      (for-each-word count! line)
      (loop))))

;; Merge sort, stable, on a vector (recursion depth log n, however many words there are).
(define (sort! less? v)
  (define tmp (vector-copy v))
  (define (merge-sort! lo hi)          ; sorts v[lo, hi)
    (when (> (- hi lo) 1)
      (let ((mid (quotient (+ lo hi) 2)))
        (merge-sort! lo mid)
        (merge-sort! mid hi)
        (vector-copy! tmp lo v lo hi)
        (let loop ((i lo) (j mid) (k lo))
          (cond ((= k hi))
                ((and (< j hi) (or (= i mid) (less? (vector-ref tmp j) (vector-ref tmp i))))
                 (vector-set! v k (vector-ref tmp j))
                 (loop i (+ j 1) (+ k 1)))
                (else
                 (vector-set! v k (vector-ref tmp i))
                 (loop (+ i 1) j (+ k 1))))))))
  (merge-sort! 0 (vector-length v))
  v)

;; Most frequent first; equal counts in alphabetical order.
(define ranked
  (sort! (lambda (a b)
           (or (> (cdr a) (cdr b))
               (and (= (cdr a) (cdr b)) (string<? (car a) (car b)))))
         (list->vector (apply append (vector->list buckets)))))

(do ((i 0 (+ i 1)))
    ((or (= i top) (= i (vector-length ranked))))
  (let* ((entry (vector-ref ranked i))
         (count (number->string (cdr entry))))
    (display (make-string (max 0 (- 7 (string-length count))) #\space))
    (display count)
    (display "  ")
    (display (car entry))
    (newline)))
