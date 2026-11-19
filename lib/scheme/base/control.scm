;; Control features (R7RS 6.10). %apply and %call-with-values are engine-specific
;; control primitives (DESIGN.md §3). TODO(M1): call/cc, dynamic-wind.
(export procedure? apply map for-each string-map string-for-each vector-map vector-for-each
        values call-with-values)
(begin
  (define (procedure? x) (%procedure? x))
  (define (apply-args x rest)            ; (a b ... lst) -> (a b ... . lst)
    (if (%null? rest) x (%cons x (apply-args (%car rest) (%cdr rest)))))
  (define apply
    (case-lambda
      ((f args) (%apply f args))
      ((f a . rest) (%apply f (apply-args a rest)))))
  (define values
    (case-lambda
      ((x) x)
      (() (%values))
      ((a b) (%values a b))
      ((a b c) (%values a b c))
      (xs (%make-values xs))))
  (define (call-with-values producer consumer) (%call-with-values producer consumer))
  (define (any-null? ls)
    (if (%null? ls) #f (if (%pair? (%car ls)) (any-null? (%cdr ls)) #t)))
  (define (cars ls) (if (%null? ls) '() (%cons (%car (%car ls)) (cars (%cdr ls)))))
  (define (cdrs ls) (if (%null? ls) '() (%cons (%cdr (%car ls)) (cdrs (%cdr ls)))))
  (define map
    (case-lambda
      ((f l)
       (let loop ((l l) (acc '()))
         (if (%pair? l) (loop (%cdr l) (%cons (f (%car l)) acc)) (%reverse acc))))
      ((f l1 l2)
       (let loop ((l1 l1) (l2 l2) (acc '()))
         (if (if (%pair? l1) (%pair? l2) #f)
             (loop (%cdr l1) (%cdr l2) (%cons (f (%car l1) (%car l2)) acc))
             (%reverse acc))))
      ((f . ls)
       (let loop ((ls ls) (acc '()))
         (if (any-null? ls)
             (%reverse acc)
             (loop (cdrs ls) (%cons (%apply f (cars ls)) acc)))))))
  (define for-each
    (case-lambda
      ((f l)
       (let loop ((l l))
         (if (%pair? l) (begin (f (%car l)) (loop (%cdr l))))))
      ((f . ls)
       (let loop ((ls ls))
         (if (any-null? ls) #t (begin (%apply f (cars ls)) (loop (cdrs ls))))))))
  (define (string->list* s) (%string->list s 0 (%string-length s)))
  (define (vector->list* v) (%vector->list v 0 (%vector-length v)))
  (define string-map
    (case-lambda
      ((f s) (%list->string (map f (string->list* s))))
      ((f . ss) (%list->string (%apply map (%cons f (map string->list* ss)))))))
  (define string-for-each
    (case-lambda
      ((f s) (for-each f (string->list* s)))
      ((f . ss) (%apply for-each (%cons f (map string->list* ss))))))
  (define vector-map
    (case-lambda
      ((f v) (%list->vector (map f (vector->list* v))))
      ((f . vs) (%list->vector (%apply map (%cons f (map vector->list* vs)))))))
  (define vector-for-each
    (case-lambda
      ((f v) (for-each f (vector->list* v)))
      ((f . vs) (%apply for-each (%cons f (map vector->list* vs)))))))
