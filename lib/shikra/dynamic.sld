;; (shikra dynamic) -- internal: the walk over the dynamic state that every non-local jump
;; performs (spec §8.4, R7RS 6.10). Shared by call/cc and guard in (scheme base) and by exit
;; in (scheme process-context); plain Scheme on %primitives, so both engines use it.
;;
;; The winders list (runtime global, innermost first) holds two kinds of frames:
;;   (before . after)  dynamic-wind: car is a procedure
;;   (cell . value)    parameterize: car is the parameter's cell (a pair)
;; Parameter values are looked up in the list (%parameter-binding), so restoring a list
;; restores every parameter at once; only dynamic-wind frames run code.
(define-library (shikra dynamic)
  (import (shikra primitives))
  (export travel-to!)
  (begin
    (define (drop l k) (if (%> k 0) (drop (%cdr l) (%- k 1)) l))
    (define (common-tail a b)
      (let ((la (%length a)) (lb (%length b)))
        (let loop ((a (drop a (%- la lb))) (b (drop b (%- lb la))))
          (if (%eq? a b) a (loop (%cdr a) (%cdr b))))))
    ;; Make `to` the current winders list: unwind to the common ancestor, calling each
    ;; after thunk with the winders outside its frame, then rewind outermost first, calling
    ;; each before thunk outside its frame and entering the frame after it returns.
    (define (travel-to! to)
      (let ((from (%winders)))
        (if (%eq? from to)
            #f
            (let ((common (common-tail from to)))
              (let unwind ((w from))
                (if (%eq? w common)
                    #f
                    (begin
                      (%set-winders! (%cdr w))
                      (if (%procedure? (%car (%car w))) ((%cdr (%car w))))
                      (unwind (%cdr w)))))
              (let rewind ((w to))
                (if (%eq? w common)
                    #f
                    (begin
                      (rewind (%cdr w))
                      (if (%procedure? (%car (%car w))) ((%car (%car w))))
                      (%set-winders! w))))))))))
