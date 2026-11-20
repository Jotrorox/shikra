;; util/queue.sld: a FIFO queue, as a library of its own. A program in the directory above
;; imports it as (util queue): Shikra looks for (a b) as a/b.sld in the program's directory,
;; then in each -I directory, then among its built-in libraries.
(define-library (util queue)
  (export make-queue queue-empty? enqueue! dequeue!)
  (import (scheme base))
  (begin
    ;; front: a list; back: its last pair, so enqueue! is O(1).
    (define-record-type queue
      (raw-queue front back)
      queue?
      (front queue-front set-queue-front!)
      (back queue-back set-queue-back!))

    (define (make-queue) (raw-queue '() '()))

    (define (queue-empty? q) (null? (queue-front q)))

    (define (enqueue! q x)
      (let ((cell (list x)))
        (if (null? (queue-front q))
            (set-queue-front! q cell)
            (set-cdr! (queue-back q) cell))
        (set-queue-back! q cell)))

    (define (dequeue! q)
      (when (queue-empty? q) (error "dequeue!: empty queue"))
      (let ((x (car (queue-front q))))
        (set-queue-front! q (cdr (queue-front q)))
        x))))
