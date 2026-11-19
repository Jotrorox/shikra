;; R7RS 6.10 dynamic-wind example with re-entry (tier 3, spec §8.2): the continuation is
;; stored and invoked after its call/cc returned; re-entering the dynamic-wind body runs the
;; before thunk again. Both engines print the R7RS result.
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
