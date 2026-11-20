;; maze.scm: the shortest path through a maze, by breadth-first search.
;;
;;   shikra run maze.scm                   # the maze below
;;   shikra run maze.scm my-maze.txt       # '#' walls, 'S' start, 'E' end
;;
;; Shows: a library of your own (util/queue.sld, next to this file), strings as a mutable
;; grid, and reading a file named on the command line.
(import (scheme base) (scheme file) (scheme write) (scheme process-context) (util queue))

(define built-in
  '("##########################"
    "#S     #        #        #"
    "# #### # ###### # ###### #"
    "# #    #      # #      # #"
    "# # ######### # ###### # #"
    "# #         # #        # #"
    "# ######### # ########## #"
    "#         #              #"
    "######### # ############ #"
    "#         #            #E#"
    "##########################"))

(define (read-maze file)
  (call-with-input-file file
    (lambda (port)
      (let loop ((lines '()))
        (let ((line (read-line port)))
          (if (eof-object? line)
              (reverse lines)
              (loop (cons line lines))))))))

;; The grid is a vector of mutable strings (literals are immutable, hence string-copy).
(define grid
  (list->vector
   (map string-copy
        (let ((args (cdr (command-line))))
          (if (pair? args) (read-maze (car args)) built-in)))))

(define rows (vector-length grid))
(define (cell r c)
  (if (and (< -1 r rows) (< -1 c (string-length (vector-ref grid r))))
      (string-ref (vector-ref grid r) c)
      #\#))

(define (find ch)
  (let loop ((r 0) (c 0))
    (cond ((= r rows) (error "maze has no" ch))
          ((= c (string-length (vector-ref grid r))) (loop (+ r 1) 0))
          ((char=? (cell r c) ch) (cons r c))
          (else (loop r (+ c 1))))))

(define start (find #\S))
(define end (find #\E))

;; came-from[r][c]: the cell the search reached (r . c) from, or #f while it is unvisited.
(define came-from (make-vector rows #f))
(do ((r 0 (+ r 1))) ((= r rows))
  (vector-set! came-from r (make-vector (string-length (vector-ref grid r)) #f)))
(define (visited? p) (vector-ref (vector-ref came-from (car p)) (cdr p)))
(define (visit! p from) (vector-set! (vector-ref came-from (car p)) (cdr p) from))

(define (search)
  (let ((q (make-queue)))
    (enqueue! q start)
    (visit! start 'start)
    (let loop ()
      (if (queue-empty? q)
          #f
          (let ((p (dequeue! q)))
            (if (equal? p end)
                #t
                (begin
                  (for-each
                   (lambda (d)
                     (let ((n (cons (+ (car p) (car d)) (+ (cdr p) (cdr d)))))
                       (unless (or (char=? (cell (car n) (cdr n)) #\#) (visited? n))
                         (visit! n p)
                         (enqueue! q n))))
                   '((0 . 1) (1 . 0) (0 . -1) (-1 . 0)))
                  (loop))))))))

(cond ((search)
       ;; walk back from the end, marking the path
       (let loop ((p (visited? end)) (steps 1))
         (if (equal? p start)
             (begin
               (vector-for-each (lambda (row) (display row) (newline)) grid)
               (display steps) (display " steps") (newline))
             (begin
               (string-set! (vector-ref grid (car p)) (cdr p) #\.)
               (loop (visited? p) (+ steps 1))))))
      (else (display "no path") (newline)))
