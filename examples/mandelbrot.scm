;; mandelbrot.scm: the Mandelbrot set as text.
;;
;;   shikra run mandelbrot.scm             # 72 x 28 characters
;;   shikra run mandelbrot.scm 150 50      # width and height
;;
;; Plain R7RS arithmetic, no type declarations: the compiler infers that the inner loop works
;; on flonums only and keeps them unboxed, so the loop allocates nothing. (The extension
;; library (shikra flonum) is for code where it cannot infer that.)
(import (scheme base) (scheme inexact) (scheme process-context))

(define args (cdr (command-line)))
(define width (if (pair? args) (string->number (car args)) 72))
(define height (if (and (pair? args) (pair? (cdr args))) (string->number (cadr args)) 28))
(define max-iterations 200)

;; The number of iterations before z = z^2 + c escapes, or max-iterations.
(define (escape-time cr ci)
  (let loop ((zr 0.0) (zi 0.0) (n 0))
    (cond ((= n max-iterations) n)
          ((> (+ (* zr zr) (* zi zi)) 4.0) n)
          (else (loop (+ (- (* zr zr) (* zi zi)) cr)
                      (+ (* 2.0 zr zi) ci)
                      (+ n 1))))))

(define shades " .:-=+*#%@")

(define (shade n)
  (if (= n max-iterations)
      #\@
      (string-ref shades (min 8 (exact (floor (* 1.5 (log (+ n 1)))))))))

(do ((y 0 (+ y 1))) ((= y height))
  (let ((ci (- (* 2.4 (/ y (- height 1))) 1.2)))
    (do ((x 0 (+ x 1))) ((= x width))
      (let ((cr (- (* 3.2 (/ x (- width 1))) 2.2)))
        (write-char (shade (escape-time cr ci)))))
    (newline)))
