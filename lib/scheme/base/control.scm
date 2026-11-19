;; Control features (R7RS 4.2.6, 6.10; spec §8). %apply, %values, %call-with-values and
;; %call/cc are the engine-specific control operations (DESIGN.md §3); everything else is
;; Scheme on %primitives: dynamic-wind and parameterize frames live in one winders list
;; (see (shikra dynamic)), continuations rewind it before they jump.
(export procedure? apply map for-each string-map string-for-each vector-map vector-for-each
        values call-with-values call-with-current-continuation call/cc dynamic-wind
        make-parameter parameterize features)
(begin
  (define (dynamic-wind before thunk after)
    (before)
    (let ((w (%winders)))
      (%set-winders! (%cons (%cons before after) w))
      (let ((r (thunk)))
        (%set-winders! w)
        (after)
        r)))
  ;; call/cc, tier 2 (spec §8.2): %call/cc's k restores the handler stack and jumps; the
  ;; escape procedure first walks the winders back to the capture point. A k whose capture
  ;; has returned cannot be re-entered until tier 3 (M5). Calls whose receiver is a literal
  ;; (lambda (k) ...) that only calls k, without crossing a lambda, never get here: the
  ;; compiler turns them into jumps (tier 1).
  (define (call-with-current-continuation f)
    (%call/cc (lambda (k) (f (escape-procedure k (%winders))))))
  (define (call/cc f)
    (%call/cc (lambda (k) (f (escape-procedure k (%winders))))))
  (define (escape-procedure k w)
    (lambda vals
      (if (%continuation-live? k)
          (begin (travel-to! w) (%apply k vals))
          (error "re-entrant continuation not supported yet (milestone M5)"))))
  ;; Parameter objects are procedures (closures). The cell (converter . global-value) is
  ;; their identity: a parameterize frame (cell . value) in the winders list shadows it.
  ;; Called with the private key, a parameter returns its cell; with another argument it
  ;; sets the current binding (R7RS leaves this unspecified).
  (define parameter-key '(parameter-key))
  (define make-parameter
    (case-lambda
      ((value) (make-parameter* value #f))
      ((value converter) (make-parameter* value converter))))
  (define (make-parameter* value converter)
    (let ((cell (%cons converter (if converter (converter value) value))))
      (case-lambda
        (() (%cdr (%parameter-binding cell)))
        ((x) (if (%eq? x parameter-key)
                 cell
                 (%set-cdr! (%parameter-binding cell) (if converter (converter x) x)))))))
  (define (parameter-cell p)
    (let ((cell (if (%procedure? p) (p parameter-key) #f)))
      (if (%pair? cell) cell (type-error "parameterize" "parameter" p))))
  ;; Values are converted before any binding takes effect; restoring does not convert.
  ;; The three port parameters are shallow-bound: their frame is a dynamic-wind frame whose
  ;; before and after thunks swap the value in and out of the static cell, so the current
  ;; port is always the cell's cdr; travel-to! enters and leaves the frames, also on escapes.
  (define-syntax parameterize
    (syntax-rules ()
      ((_ () body ...) (let () body ...))
      ((_ ((p v) ...) body ...)
       (parameterize-run (list p ...) (list v ...) (lambda () body ...)))))
  (define (port-cell? c)
    (if (%eq? c (%port-cell 0)) #t (if (%eq? c (%port-cell 1)) #t (%eq? c (%port-cell 2)))))
  (define (swap-frame cell v)
    (let ((swap (lambda () (let ((t (%cdr cell))) (%set-cdr! cell v) (set! v t)))))
      (%cons swap swap)))
  (define (parameterize-run params vals thunk)
    (let ((w (%winders)))
      (let loop ((ps params) (vs vals) (acc w))
        (if (%null? ps)
            (begin
              (travel-to! acc)
              (let ((r (thunk)))
                (travel-to! w)
                r))
            (let* ((cell (parameter-cell (%car ps)))
                   (conv (%car cell))
                   (v (if conv (conv (%car vs)) (%car vs))))
              (loop (%cdr ps) (%cdr vs)
                    (%cons (if (port-cell? cell) (swap-frame cell v) (%cons cell v)) acc)))))))
  (define (procedure? x) (%procedure? x))
  ;; R7RS 6.14: the cond-expand feature identifiers (spec 3.4), fixed at compile time.
  (define (features) (list-copy (%features)))
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
  ;; One vector: an index loop (inlines to a C loop specialised to f). The results are
  ;; collected in a list, so a continuation that returns twice does not mutate a vector
  ;; already returned (R7RS 6.10).
  (define vector-map
    (case-lambda
      ((f v)
       (let ((n (%vector-length v)))
         (let loop ((i 0) (acc '()))
           (if (%< i n)
               (loop (%+ i 1) (%cons (f (%vector-ref v i)) acc))
               (%list->vector (%reverse acc))))))
      ((f . vs) (%list->vector (%apply map (%cons f (map vector->list* vs)))))))
  (define vector-for-each
    (case-lambda
      ((f v)
       (let ((n (%vector-length v)))
         (let loop ((i 0))
           (if (%< i n) (begin (f (%vector-ref v i)) (loop (%+ i 1)))))))
      ((f . vs) (%apply for-each (%cons f (map vector->list* vs)))))))
