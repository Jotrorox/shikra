;; (scheme base) for Chez Scheme 10 -- a minimal R7RS-small compatibility layer
;; written for the Shikra benchmark harness (bench/run.sh), so that the
;; r7rs-benchmarks programs run unmodified under `chez --program`.
;;
;; Everything Chez already provides with R7RS semantics is re-exported as is
;; (so Chez's own optimizer sees its own primitives).  Only procedures that are
;; missing from Chez, or whose Chez signature differs from R7RS (R6RS argument
;; order of the *-copy! procedures, start/end arguments, Chez's 3-argument
;; error, member/assoc with a predicate, R7RS define-record-type syntax), are
;; defined here.  Known gaps: `case` with `=>` and `include-ci` are not
;; provided; `map`/`for-each` keep Chez's R6RS behaviour on lists of unequal
;; length.
(library (scheme base)
  (export
   * + - ... / < <= = => > >= _
   abs and append apply assoc assq assv begin binary-port? boolean=? boolean?
   bytevector bytevector-append bytevector-copy bytevector-copy! bytevector-length
   bytevector-u8-ref bytevector-u8-set! bytevector? caar cadr
   call-with-current-continuation call-with-port call-with-values call/cc car case
   cdar cddr cdr ceiling char->integer char-ready? char<=? char<? char=? char>=?
   char>? char? close-input-port close-output-port close-port complex? cond
   cond-expand cons current-error-port current-input-port current-output-port
   define define-record-type define-syntax define-values denominator do
   dynamic-wind else eof-object eof-object? eq? equal? eqv? error
   error-object-irritants error-object-message error-object? even? exact
   exact-integer-sqrt exact-integer? exact? expt features file-error? floor
   floor-quotient floor-remainder floor/ flush-output-port for-each gcd
   get-output-bytevector get-output-string guard if include inexact inexact?
   input-port-open? input-port? integer->char integer? lambda lcm length let let*
   let*-values let-syntax let-values letrec letrec* letrec-syntax list list->string
   list->vector list-copy list-ref list-set! list-tail list? make-bytevector
   make-list make-parameter make-string make-vector map max member memq memv min
   modulo negative? newline not null? number->string number? numerator odd?
   open-input-bytevector open-input-string open-output-bytevector
   open-output-string or output-port-open? output-port? pair? parameterize
   peek-char peek-u8 port? positive? procedure? quasiquote quote quotient raise
   raise-continuable rational? rationalize read-bytevector read-bytevector!
   read-char read-error? read-line read-string read-u8 real? remainder reverse
   round set! set-car! set-cdr! square string string->list string->number
   string->symbol string->utf8 string->vector string-append string-copy
   string-copy! string-fill! string-for-each string-length string-map string-ref
   string-set! string<=? string<? string=? string>=? string>? string? substring
   symbol->string symbol=? symbol? syntax-error syntax-rules textual-port?
   truncate truncate-quotient truncate-remainder truncate/ u8-ready? unless
   unquote unquote-splicing utf8->string values vector vector->list vector->string
   vector-append vector-copy vector-copy! vector-fill! vector-for-each
   vector-length vector-map vector-ref vector-set! vector? when
   with-exception-handler write-bytevector write-char write-string write-u8 zero?)
  (import
   (except (chezscheme)
           assoc member error define-record-type syntax-error
           string-copy string->list string-copy! string-fill!
           vector->list vector-copy vector-copy! vector-fill!
           bytevector-copy bytevector-copy! utf8->string string->utf8)
   (prefix (only (chezscheme)
                 assoc member error define-record-type syntax-error
                 string-copy string->list string-copy! string-fill!
                 vector->list vector-copy vector-copy! vector-fill!
                 bytevector-copy bytevector-copy! utf8->string string->utf8)
           chez:))

  ;; ---- syntax -------------------------------------------------------------

  ;; R7RS: (define-record-type <name> (<ctor> <field> ...) <pred>
  ;;         (<field> <accessor> [<modifier>]) ...)
  ;; Expands to Chez's native R6RS define-record-type so that accessors and
  ;; the predicate are inlined exactly as in native Chez code.
  (define-syntax define-record-type
    (lambda (x)
      (define (field-clause spec)
        (syntax-case spec ()
          ((name acc) #'(immutable name acc))
          ((name acc mod) #'(mutable name acc mod))))
      (syntax-case x ()
        ((_ type (ctor cfield ...) pred (fname facc . fmod) ...)
         (let ((cfields #'(cfield ...)))
           (with-syntax (((clause ...) (map field-clause #'((fname facc . fmod) ...)))
                         ((arg ...)
                          (map (lambda (f)
                                 (if (memp (lambda (c) (bound-identifier=? c f)) cfields)
                                     f
                                     #'#f))
                               #'(fname ...))))
             #'(begin
                 (chez:define-record-type (type make-raw pred)
                   (fields clause ...))
                 (define (ctor cfield ...) (make-raw arg ...))))))
        ((_ type #f pred fspec ...)
         #'(define-record-type type (ignored-ctor) pred fspec ...)))))

  (define-syntax syntax-error
    (lambda (x)
      (syntax-case x ()
        ((_ msg arg ...)
         (chez:syntax-error x (syntax->datum #'msg))))))

  (define (features)
    '(r7rs exact-closed exact-complex ieee-float full-unicode ratios
      chezscheme chez macosx posix))

  (define-syntax cond-expand
    (lambda (x)
      (define (true? req)
        (syntax-case req (and or not library)
          ((and r ...) (for-all true? #'(r ...)))
          ((or r ...) (exists true? #'(r ...)))
          ((not r) (not (true? #'r)))
          ((library name)
           (let ((n (syntax->datum #'name)))
             (and (pair? n) (memq (car n) '(scheme chezscheme rnrs)) #t)))
          (id (identifier? #'id)
              (and (memq (syntax->datum #'id)
                         '(r7rs exact-closed exact-complex ieee-float full-unicode
                           ratios chezscheme chez macosx posix))
                   #t))))
      (syntax-case x (else)
        ((_) #'(begin))
        ((_ (else body ...)) #'(begin body ...))
        ((_ (req body ...) more ...)
         (if (true? #'req)
             #'(begin body ...)
             #'(cond-expand more ...))))))

  ;; ---- errors -------------------------------------------------------------

  (define (error message . irritants)
    (apply chez:error #f message irritants))
  (define (error-object? x) (condition? x))
  (define (error-object-message x)
    (if (message-condition? x) (condition-message x) ""))
  (define (error-object-irritants x)
    (if (irritants-condition? x) (condition-irritants x) '()))
  (define (read-error? x)
    (and (condition? x) (or (i/o-read-error? x) (lexical-violation? x))))
  (define (file-error? x)
    (and (condition? x) (i/o-filename-error? x)))

  ;; ---- numbers ------------------------------------------------------------

  (define (exact-integer? x) (and (integer? x) (exact? x)))
  (define (floor-quotient n d)
    (let ((q (quotient n d)) (r (remainder n d)))
      (if (and (not (zero? r)) (not (eq? (negative? r) (negative? d))))
          (- q 1)
          q)))
  (define (floor-remainder n d) (modulo n d))
  (define (floor/ n d) (values (floor-quotient n d) (modulo n d)))
  (define (truncate-quotient n d) (quotient n d))
  (define (truncate-remainder n d) (remainder n d))
  (define (truncate/ n d) (values (quotient n d) (remainder n d)))
  (define (square x) (* x x))

  ;; ---- lists --------------------------------------------------------------

  (define member
    (case-lambda
      ((x l) (chez:member x l))
      ((x l same?)
       (let loop ((l l))
         (cond ((null? l) #f) ((same? x (car l)) l) (else (loop (cdr l))))))))
  (define assoc
    (case-lambda
      ((x l) (chez:assoc x l))
      ((x l same?)
       (let loop ((l l))
         (cond ((null? l) #f) ((same? x (caar l)) (car l)) (else (loop (cdr l))))))))
  (define (list-set! l k v) (set-car! (list-tail l k) v))

  ;; ---- strings ------------------------------------------------------------

  (define string-copy
    (case-lambda
      ((s) (chez:string-copy s))
      ((s start) (substring s start (string-length s)))
      ((s start end) (substring s start end))))
  (define string->list
    (case-lambda
      ((s) (chez:string->list s))
      ((s start) (chez:string->list (substring s start (string-length s))))
      ((s start end) (chez:string->list (substring s start end)))))
  (define string-copy!
    (case-lambda
      ((to at from) (string-copy! to at from 0 (string-length from)))
      ((to at from start) (string-copy! to at from start (string-length from)))
      ((to at from start end)
       (chez:string-copy! from start to at (- end start)))))
  (define string-fill!
    (case-lambda
      ((s c) (chez:string-fill! s c))
      ((s c start) (string-fill! s c start (string-length s)))
      ((s c start end)
       (do ((i start (+ i 1))) ((>= i end)) (string-set! s i c)))))
  (define (string-map f s . ss)
    (if (null? ss)
        (let* ((n (string-length s)) (r (make-string n)))
          (do ((i 0 (+ i 1))) ((= i n) r)
            (string-set! r i (f (string-ref s i)))))
        (list->string (apply map f (chez:string->list s) (map chez:string->list ss)))))
  (define string->vector
    (case-lambda
      ((s) (list->vector (chez:string->list s)))
      ((s start) (string->vector s start (string-length s)))
      ((s start end) (list->vector (string->list s start end)))))
  (define vector->string
    (case-lambda
      ((v) (list->string (chez:vector->list v)))
      ((v start) (vector->string v start (vector-length v)))
      ((v start end) (list->string (vector->list v start end)))))
  (define string->utf8
    (case-lambda
      ((s) (chez:string->utf8 s))
      ((s start) (chez:string->utf8 (substring s start (string-length s))))
      ((s start end) (chez:string->utf8 (substring s start end)))))
  (define utf8->string
    (case-lambda
      ((bv) (chez:utf8->string bv))
      ((bv start) (chez:utf8->string (bytevector-copy bv start)))
      ((bv start end) (chez:utf8->string (bytevector-copy bv start end)))))

  ;; ---- vectors ------------------------------------------------------------

  (define vector->list
    (case-lambda
      ((v) (chez:vector->list v))
      ((v start) (vector->list v start (vector-length v)))
      ((v start end)
       (let loop ((i (- end 1)) (acc '()))
         (if (< i start) acc (loop (- i 1) (cons (vector-ref v i) acc)))))))
  (define vector-copy
    (case-lambda
      ((v) (chez:vector-copy v))
      ((v start) (chez:vector-copy v start (- (vector-length v) start)))
      ((v start end) (chez:vector-copy v start (- end start)))))
  (define vector-copy!
    (case-lambda
      ((to at from) (vector-copy! to at from 0 (vector-length from)))
      ((to at from start) (vector-copy! to at from start (vector-length from)))
      ((to at from start end)
       (chez:vector-copy! from start to at (- end start)))))
  (define vector-fill!
    (case-lambda
      ((v x) (chez:vector-fill! v x))
      ((v x start) (vector-fill! v x start (vector-length v)))
      ((v x start end)
       (do ((i start (+ i 1))) ((>= i end)) (vector-set! v i x)))))

  ;; ---- bytevectors --------------------------------------------------------

  (define bytevector-copy
    (case-lambda
      ((bv) (chez:bytevector-copy bv))
      ((bv start) (bytevector-copy bv start (bytevector-length bv)))
      ((bv start end)
       (let ((r (make-bytevector (- end start))))
         (chez:bytevector-copy! bv start r 0 (- end start))
         r))))
  (define bytevector-copy!
    (case-lambda
      ((to at from) (bytevector-copy! to at from 0 (bytevector-length from)))
      ((to at from start) (bytevector-copy! to at from start (bytevector-length from)))
      ((to at from start end)
       (chez:bytevector-copy! from start to at (- end start)))))
  (define (bytevector-append . bvs)
    (let* ((n (apply + (map bytevector-length bvs)))
           (r (make-bytevector n)))
      (let loop ((bvs bvs) (i 0))
        (if (null? bvs)
            r
            (let ((k (bytevector-length (car bvs))))
              (chez:bytevector-copy! (car bvs) 0 r i k)
              (loop (cdr bvs) (+ i k)))))))

  ;; ---- ports --------------------------------------------------------------

  (define (input-port-open? p) (not (port-closed? p)))
  (define (output-port-open? p) (not (port-closed? p)))
  (define read-line
    (case-lambda (() (get-line (current-input-port))) ((p) (get-line p))))
  (define read-string
    (case-lambda
      ((k) (read-string k (current-input-port)))
      ((k p) (if (zero? k) "" (get-string-n p k)))))
  (define write-string
    (case-lambda
      ((s) (put-string (current-output-port) s))
      ((s p) (put-string p s))
      ((s p start) (put-string p s start (- (string-length s) start)))
      ((s p start end) (put-string p s start (- end start)))))
  (define read-u8
    (case-lambda (() (get-u8 (current-input-port))) ((p) (get-u8 p))))
  (define peek-u8
    (case-lambda (() (lookahead-u8 (current-input-port))) ((p) (lookahead-u8 p))))
  (define u8-ready?
    (case-lambda (() #t) ((p) #t)))
  (define write-u8
    (case-lambda ((b) (put-u8 (current-output-port) b)) ((b p) (put-u8 p b))))
  (define read-bytevector
    (case-lambda
      ((k) (get-bytevector-n (current-input-port) k))
      ((k p) (get-bytevector-n p k))))
  (define read-bytevector!
    (case-lambda
      ((bv) (read-bytevector! bv (current-input-port) 0 (bytevector-length bv)))
      ((bv p) (read-bytevector! bv p 0 (bytevector-length bv)))
      ((bv p start) (read-bytevector! bv p start (bytevector-length bv)))
      ((bv p start end) (get-bytevector-n! p bv start (- end start)))))
  (define write-bytevector
    (case-lambda
      ((bv) (put-bytevector (current-output-port) bv))
      ((bv p) (put-bytevector p bv))
      ((bv p start) (put-bytevector p bv start (- (bytevector-length bv) start)))
      ((bv p start end) (put-bytevector p bv start (- end start)))))
  (define (open-input-bytevector bv) (open-bytevector-input-port bv))
  (define bytevector-port-getters (make-weak-eq-hashtable))
  (define (open-output-bytevector)
    (let-values (((p get) (open-bytevector-output-port)))
      (hashtable-set! bytevector-port-getters p get)
      p))
  (define (get-output-bytevector p)
    (let ((get (hashtable-ref bytevector-port-getters p #f)))
      (if get
          (let ((bv (get)))
            ;; Chez's getter resets the port; R7RS keeps the contents.
            (put-bytevector p bv)
            bv)
          (error "get-output-bytevector: not a bytevector output port" p)))))
