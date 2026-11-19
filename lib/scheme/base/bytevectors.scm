;; Bytevectors (R7RS 6.9).
(export bytevector? make-bytevector bytevector bytevector-u8-ref bytevector-u8-set!
        bytevector-length bytevector-copy bytevector-copy! bytevector-append
        utf8->string string->utf8)
(begin
  (define (bytevector? x) (%bytevector? x))
  (define make-bytevector
    (case-lambda
      ((k) (%make-bytevector k 0))
      ((k fill) (%make-bytevector k fill))))
  (define (bytevector . bytes)
    (let ((bv (%make-bytevector (%length bytes) 0)))
      (let loop ((i 0) (l bytes))
        (if (%null? l) bv (begin (%bytevector-u8-set! bv i (%car l)) (loop (%+ i 1) (%cdr l)))))))
  (define (bytevector-u8-ref bv k) (%bytevector-u8-ref bv k))
  (define (bytevector-u8-set! bv k b) (%bytevector-u8-set! bv k b))
  (define (bytevector-length bv) (%bytevector-length bv))
  (define bytevector-copy
    (case-lambda
      ((bv) (%bytevector-copy bv 0 (%bytevector-length bv)))
      ((bv start) (%bytevector-copy bv start (%bytevector-length bv)))
      ((bv start end) (%bytevector-copy bv start end))))
  (define bytevector-copy!
    (case-lambda
      ((to at from) (%bytevector-copy! to at from 0 (%bytevector-length from)))
      ((to at from start) (%bytevector-copy! to at from start (%bytevector-length from)))
      ((to at from start end) (%bytevector-copy! to at from start end))))
  (define (bytevector-append . bvs)
    (let ((out (%make-bytevector
                (let count ((l bvs) (n 0))
                  (if (%null? l) n (count (%cdr l) (%+ n (%bytevector-length (%car l))))))
                0)))
      (let loop ((l bvs) (at 0))
        (if (%null? l)
            out
            (let ((bv (%car l)))
              (%bytevector-copy! out at bv 0 (%bytevector-length bv))
              (loop (%cdr l) (%+ at (%bytevector-length bv))))))))
  (define utf8->string
    (case-lambda
      ((bv) (%utf8->string bv))
      ((bv start) (%utf8->string (%bytevector-copy bv start (%bytevector-length bv))))
      ((bv start end) (%utf8->string (%bytevector-copy bv start end)))))
  (define string->utf8
    (case-lambda
      ((s) (%string->utf8 s))
      ((s start) (%string->utf8 (%substring s start (%string-length s))))
      ((s start end) (%string->utf8 (%substring s start end))))))
