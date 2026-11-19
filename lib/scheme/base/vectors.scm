;; Vectors (R7RS 6.8).
(export vector? make-vector vector vector-length vector-ref vector-set! vector->list
        list->vector vector-fill! vector-copy vector-copy! vector-append
        vector->string string->vector)
(begin
  (define (vector? x) (%vector? x))
  (define make-vector
    (case-lambda
      ((k) (%make-vector k #f))
      ((k fill) (%make-vector k fill))))
  (define (vector . xs) (%list->vector xs))
  (define (vector-length v) (%vector-length v))
  (define (vector-ref v k) (%vector-ref v k))
  (define (vector-set! v k x) (%vector-set! v k x))
  (define vector->list
    (case-lambda
      ((v) (%vector->list v 0 (%vector-length v)))
      ((v start) (%vector->list v start (%vector-length v)))
      ((v start end) (%vector->list v start end))))
  (define (list->vector l) (%list->vector l))
  (define vector-fill!
    (case-lambda
      ((v x) (vector-fill! v x 0 (%vector-length v)))
      ((v x start) (vector-fill! v x start (%vector-length v)))
      ((v x start end)
       (let loop ((i start))
         (if (%< i end) (begin (%vector-set! v i x) (loop (%+ i 1))))))))
  (define vector-copy
    (case-lambda
      ((v) (vector-copy v 0 (%vector-length v)))
      ((v start) (vector-copy v start (%vector-length v)))
      ((v start end) (%list->vector (%vector->list v start end)))))
  (define vector-copy!
    (case-lambda
      ((to at from) (vector-copy! to at from 0 (%vector-length from)))
      ((to at from start) (vector-copy! to at from start (%vector-length from)))
      ((to at from start end)
       (let ((tmp (%list->vector (%vector->list from start end))))
         (let loop ((i 0) (n (%- end start)))
           (if (%< i n)
               (begin (%vector-set! to (%+ at i) (%vector-ref tmp i))
                      (loop (%+ i 1) n))))))))
  (define (vector-append . vs)
    (let loop ((vs vs) (acc '()))
      (if (%null? vs)
          (%list->vector acc)
          (loop (%cdr vs)
                (%append2 acc (%vector->list (%car vs) 0 (%vector-length (%car vs))))))))
  (define vector->string
    (case-lambda
      ((v) (%list->string (%vector->list v 0 (%vector-length v))))
      ((v start) (%list->string (%vector->list v start (%vector-length v))))
      ((v start end) (%list->string (%vector->list v start end)))))
  (define string->vector
    (case-lambda
      ((s) (%list->vector (%string->list s 0 (%string-length s))))
      ((s start) (%list->vector (%string->list s start (%string-length s))))
      ((s start end) (%list->vector (%string->list s start end))))))
