;; Strings (R7RS 6.7): UTF-8 with an ASCII fast path in the runtime.
(export string? make-string string string-length string-ref string-set! substring
        string-append string-copy string-copy! string-fill! string->list list->string
        string=? string<? string>? string<=? string>=?)
(begin
  (define (string? x) (%string? x))
  (define make-string
    (case-lambda
      ((k) (%make-string k #\space))
      ((k c) (%make-string k c))))
  (define (string . chars) (%list->string chars))
  (define (string-length s) (%string-length s))
  (define (string-ref s k) (%string-ref s k))
  (define (string-set! s k c) (%string-set! s k c))
  (define (substring s start end) (%substring s start end))
  (define string-append
    (case-lambda
      (() "")
      ((a) (%string-append a ""))
      ((a b) (%string-append a b))
      ((a . rest)
       (let loop ((acc a) (rest rest))
         (if (%null? rest) acc (loop (%string-append acc (%car rest)) (%cdr rest)))))))
  (define string-copy
    (case-lambda
      ((s) (%substring s 0 (%string-length s)))
      ((s start) (%substring s start (%string-length s)))
      ((s start end) (%substring s start end))))
  (define string-copy!
    (case-lambda
      ((to at from) (string-copy! to at from 0 (%string-length from)))
      ((to at from start) (string-copy! to at from start (%string-length from)))
      ((to at from start end)
       (let ((tmp (%substring from start end)))
         (let loop ((i 0) (n (%- end start)))
           (if (%< i n)
               (begin (%string-set! to (%+ at i) (%string-ref tmp i))
                      (loop (%+ i 1) n))))))))
  (define string-fill!
    (case-lambda
      ((s c) (string-fill! s c 0 (%string-length s)))
      ((s c start) (string-fill! s c start (%string-length s)))
      ((s c start end)
       (let loop ((i start))
         (if (%< i end) (begin (%string-set! s i c) (loop (%+ i 1))))))))
  (define string->list
    (case-lambda
      ((s) (%string->list s 0 (%string-length s)))
      ((s start) (%string->list s start (%string-length s)))
      ((s start end) (%string->list s start end))))
  (define (list->string l) (%list->string l))
  (define-syntax define-string-comparison
    (syntax-rules ()
      ((_ name prim)
       (define (name a b . rest)
         (let loop ((a a) (b b) (rest rest))
           (if (prim (%string-compare a b) 0)
               (if (%null? rest) #t (loop b (%car rest) (%cdr rest)))
               #f))))))
  (define-string-comparison string=? %=)
  (define-string-comparison string<? %<)
  (define-string-comparison string>? %>)
  (define-string-comparison string<=? %<=)
  (define-string-comparison string>=? %>=))
