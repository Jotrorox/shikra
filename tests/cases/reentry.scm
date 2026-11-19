;; R7RS 6.10 dynamic-wind example with re-entry: needs tier 3 continuations (M5). Until then
;; invoking a continuation whose call/cc has returned raises a clear error.
(import (scheme base) (scheme write))
(write (let ((path '())
             (c #f))
         (let ((add (lambda (s) (set! path (cons s path)))))
           (dynamic-wind
            (lambda () (add 'connect))
            (lambda () (add (call-with-current-continuation (lambda (c0) (set! c c0) 'talk1))))
            (lambda () (add 'disconnect)))
           (if (< (length path) 4)
               (c 'talk2)
               (reverse path)))))
(newline)
