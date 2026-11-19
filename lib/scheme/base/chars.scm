;; Characters (R7RS 6.6), the part in (scheme base). (scheme char) comes later.
(export char? char->integer integer->char char=? char<? char>? char<=? char>=?)
(begin
  (define (char? x) (%char? x))
  (define (char->integer c) (%char->integer c))
  (define (integer->char n) (%integer->char n))
  (define-syntax define-char-comparison
    (syntax-rules ()
      ((_ name prim)
       (define (name a b . rest)
         (let loop ((x (%char->integer a)) (y (%char->integer b)) (rest rest))
           (if (prim x y)
               (if (%null? rest) #t (loop y (%char->integer (%car rest)) (%cdr rest)))
               #f))))))
  (define-char-comparison char=? %=)
  (define-char-comparison char<? %<)
  (define-char-comparison char>? %>)
  (define-char-comparison char<=? %<=)
  (define-char-comparison char>=? %>=))
