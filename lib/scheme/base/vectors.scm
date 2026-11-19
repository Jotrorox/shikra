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
       (check-range "vector-fill!" (%vector-length v) start end)
       (let loop ((i start))
         (if (%< i end) (begin (%vector-set! v i x) (loop (%+ i 1))))))))
  (define vector-copy
    (case-lambda
      ((v) (vector-copy v 0 (%vector-length v)))
      ((v start) (vector-copy v start (%vector-length v)))
      ((v start end)
       (check-range "vector-copy" (%vector-length v) start end)
       (let ((out (%make-vector (%- end start) #f)))
         (vector-move! out 0 v start end)
         out))))
  ;; Copies v[start, end) to out[at, ...); the direction makes overlapping copies safe.
  (define (vector-move! out at v start end)
    (if (%<= at start)
        (let loop ((i start) (j at))
          (if (%< i end)
              (begin (%vector-set! out j (%vector-ref v i)) (loop (%+ i 1) (%+ j 1)))))
        (let loop ((i end) (j (%+ at (%- end start))))
          (if (%> i start)
              (begin (%vector-set! out (%- j 1) (%vector-ref v (%- i 1))) (loop (%- i 1) (%- j 1)))))))
  (define vector-copy!
    (case-lambda
      ((to at from) (vector-copy! to at from 0 (%vector-length from)))
      ((to at from start) (vector-copy! to at from start (%vector-length from)))
      ((to at from start end)
       (check-range "vector-copy!" (%vector-length from) start end)
       ;; 0 <= at <= (length to) - (end - start)
       (check-range "vector-copy!" (%- (%vector-length to) (%- end start)) at at)
       (vector-move! to at from start end))))
  (define (vector-append . vs)
    (let ((out (%make-vector
                (let count ((l vs) (n 0))
                  (if (%null? l) n (count (%cdr l) (%+ n (%vector-length (%car l))))))
                #f)))
      (let loop ((l vs) (at 0))
        (if (%null? l)
            out
            (let ((v (%car l)))
              (vector-move! out at v 0 (%vector-length v))
              (loop (%cdr l) (%+ at (%vector-length v))))))))
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
