;; (shikra flonum): the functions SRFI 144 adds to the platform's exp, log, sin, cos, tan, asin,
;; acos, atan, pow and sqrt -- own implementations in portable Scheme on the flonum primitives
;; (compiled to unboxed C). Accuracy measured against the host libm: DESIGN.md "Extensions as built".
(begin
  ;; x = m * 2^e exactly, m in [1, 2), for finite nonzero x
  (define (fl-scale x e) (%make-flonum x e))

  ;; exp(x) - 1: Kahan's trick near 0 (the rounding errors of exp(x) and of u - 1 cancel in
  ;; (u - 1) * x / log(u))
  (define (flexp-1 x)
    (if (%fl<? (%flabs x) 0.5)
        (let ((u (%flexp x)))
          (cond ((%fl=? u 1.0) x)
                (else (let ((um1 (%fl- u 1.0)))
                        (if (%fl=? um1 -1.0) -1.0 (%fl/ (%fl* um1 x) (%fllog u)))))))
        (%fl- (%flexp x) 1.0)))
  ;; log(1 + x): the same trick, log(u) * x / (u - 1) with u = 1 + x
  (define (fllog1+ x)
    (if (%fl<? (%flabs x) 0.5)
        (let ((u (%fl+ 1.0 x)))
          (if (%fl=? u 1.0) x (%fl/ (%fl* (%fllog u) x) (%fl- u 1.0))))
        (%fllog (%fl+ 1.0 x))))
  ;; log2: x = f * 2^e with f in [sqrt 1/2, sqrt 2): e + log(f) / log 2
  (define (fllog2 x)
    (if (if (%fl>? x 0.0) (%fl<? x +inf.0) #f)
        (let* ((e (%flinteger-exponent x)) (f (fl-scale x (%- 0 e))))
          (if (%fl>? f 1.4142135623730951)
              (fllog2-split (%fl* f 0.5) (%+ e 1))
              (fllog2-split f e)))
        (%fllog x)))
  (define (fllog2-split f e)
    (%fl+ (%inexact e) (%fl* (fllog1+ (%fl- f 1.0)) 1.4426950408889634)))
  (define (fllog10 x) (%fl* (%fllog x) 0.4342944819032518))

  ;; cube root: exp(log(m)/3) on m in [1, 8), one Newton step with the residual y^3 - m computed
  ;; exactly enough by fused multiply-adds
  (define (flcbrt x)
    (if (if (%fl=? x 0.0) #t (if (%fl=? x x) (%fl=? (%flabs x) +inf.0) #t))
        x
        (let* ((a (%flabs x))
               (e (%flinteger-exponent a))
               (k (if (%>= e 0) (%quotient e 3) (%- 0 (%quotient (%- 2 e) 3))))
               (m (fl-scale a (%* -3 k)))
               (y (%flexp (%fl/ (%fllog m) 3.0)))
               (hi (%fl* y y)) (lo (%fl+* y y (%fl- 0.0 hi)))
               (c (%fl* hi y)) (cl (%fl+* hi y (%fl- 0.0 c)))
               (r (%fl+ (%fl+ (%fl- c m) cl) (%fl* lo y)))
               (y (%fl- y (%fl/ r (%fl* 3.0 hi)))))
          (%flcopysign (fl-scale y k) x))))

  ;; sqrt(x^2 + y^2) without overflow or underflow: scaled by a power of two
  (define (flhypot x y)
    (let ((a (%flabs x)) (b (%flabs y)))
      (cond ((if (%fl=? a +inf.0) #t (%fl=? b +inf.0)) +inf.0)
            ((if (%fl=? a a) (%fl=? b b) #f)
             (let ((a (%flmax a b)) (b (%flmin a b)))
               (if (%fl=? b 0.0)
                   a
                   (let* ((e (%flinteger-exponent a)) (as (fl-scale a (%- 0 e))) (bs (fl-scale b (%- 0 e))))
                     (fl-scale (%flsqrt (%fl+* as as (%fl* bs bs))) e)))))
            (else (%fl+ a b)))))

  ;; hyperbolic functions: expm1 near 0, exp(|x|/2) squared near overflow
  (define (flsinh x)
    (let ((a (%flabs x)))
      (%flcopysign
       (cond ((%fl<? a 1.0) (let ((e (flexp-1 a))) (%fl* 0.5 (%fl+ e (%fl/ e (%fl+ e 1.0))))))
             ((%fl<? a 22.0) (let ((e (%flexp a))) (%fl* 0.5 (%fl- e (%fl/ 1.0 e)))))
             ((%fl<? a 709.0) (%fl* 0.5 (%flexp a)))
             (else (let ((t (%flexp (%fl* 0.5 a)))) (%fl* (%fl* 0.5 t) t))))
       x)))
  (define (flcosh x)
    (let ((a (%flabs x)))
      (cond ((%fl<? a 1.0) (let ((e (flexp-1 a))) (%fl+ 1.0 (%fl/ (%fl* e e) (%fl* 2.0 (%fl+ 1.0 e))))))
            ((%fl<? a 22.0) (let ((e (%flexp a))) (%fl* 0.5 (%fl+ e (%fl/ 1.0 e)))))
            ((%fl<? a 709.0) (%fl* 0.5 (%flexp a)))
            (else (let ((t (%flexp (%fl* 0.5 a)))) (%fl* (%fl* 0.5 t) t))))))
  (define (fltanh x)
    (let ((a (%flabs x)))
      (%flcopysign
       (cond ((%fl<? a 1.0) (let ((e (flexp-1 (%fl* 2.0 a)))) (%fl/ e (%fl+ e 2.0))))
             ((%fl<? a 22.0) (%fl- 1.0 (%fl/ 2.0 (%fl+ (%flexp (%fl* 2.0 a)) 1.0))))
             ((%fl=? a a) 1.0)
             (else a))
       x)))
  (define (flasinh x)
    (let ((a (%flabs x)))
      (%flcopysign
       (cond ((%fl>? a 268435456.0) (%fl+ (%fllog a) 0.6931471805599453))
             ((%fl>? a 2.0) (%fllog (%fl+ (%fl* 2.0 a) (%fl/ 1.0 (%fl+ (%flsqrt (%fl+* a a 1.0)) a)))))
             (else (let ((t (%fl* a a))) (fllog1+ (%fl+ a (%fl/ t (%fl+ 1.0 (%flsqrt (%fl+ 1.0 t)))))))))
       x)))
  (define (flacosh x)
    (cond ((%fl<? x 1.0) +nan.0)
          ((%fl>? x 268435456.0) (%fl+ (%fllog x) 0.6931471805599453))
          ((%fl>? x 2.0) (%fllog (%fl- (%fl* 2.0 x) (%fl/ 1.0 (%fl+ x (%flsqrt (%fl+* x x -1.0)))))))
          ((%fl>=? x 1.0) (let ((t (%fl- x 1.0))) (fllog1+ (%fl+ t (%flsqrt (%fl+* 2.0 t (%fl* t t)))))))
          (else x)))
  (define (flatanh x)
    (let ((a (%flabs x)))
      (%flcopysign
       (if (%fl<? a 0.5)
           (%fl* 0.5 (fllog1+ (%fl+ (%fl* 2.0 a) (%fl/ (%fl* (%fl* 2.0 a) a) (%fl- 1.0 a)))))
           (%fl* 0.5 (fllog1+ (%fl/ (%fl* 2.0 a) (%fl- 1.0 a)))))
       x)))

  ;; ---- gamma and log-gamma ----
  ;; sin(pi x) with the argument reduced exactly: x - round(x) in [-1/2, 1/2]
  (define (fl-sinpi x)
    (let* ((n (%flround x)) (r (%fl- x n)) (s (%flsin (%fl* 3.141592653589793 r))))
      (if (%fl=? (%flremainder n 2.0) 0.0) s (%fl- 0.0 s))))
  ;; Stirling's series for log Gamma(w) - ((w - 1/2) log w - w + log(2 pi)/2), w >= 12
  (define fl-stirling-c
    '#(0.08333333333333333 -0.002777777777777778 0.0007936507936507937 -0.0005952380952380953
       0.0008417508417508417 -0.0019175269175269176 0.00641025641025641 -0.029550653594771242
       0.17964437236883057 -1.3924322169059011 13.402864044168393 -156.84828462600203))
  (define (fl-stirling w)
    (let ((z (%fl/ 1.0 (%fl* w w))))
      (let loop ((k 11) (s 0.0))
        (if (%< k 0) (%fl/ s w) (loop (%- k 1) (%fl+* s z (%vector-ref fl-stirling-c k)))))))
  ;; Gamma(x) for x >= 1/2: shifted up to w = x + n >= 12, Gamma(w) = w^(w-1/2) e^-w sqrt(2 pi)
  ;; e^stirling with w^(w-1/2) as a square of pow(w, (w-1/2)/2) (no overflow before the end); the
  ;; rounding of x + n is corrected by psi(w) * (the lost part)
  (define (fl-gamma+ x)
    (let loop ((w x) (p 1.0) (n 0))
      (if (%fl<? w 12.0)
          (loop (%fl+ w 1.0) (%fl* p w) (%+ n 1))
          (let* ((lost (%fl- (%fl- w (%inexact n)) x))
                 (h (%flexpt w (%fl* 0.5 (%fl- w 0.5))))
                 (g (%fl* (%fl* h (%fl* h (%flexp (%fl- 0.0 w)))) (%fl* 2.5066282746310002 (%flexp (fl-stirling w)))))
                 (g (%fl- g (%fl* (%fl* g lost) (%fl- (%fllog w) (%fl/ 0.5 w))))))
            (%fl/ g p)))))
  (define (flgamma x)
    (cond ((if (%fl=? x x) #f #t) x)
          ((%fl=? x +inf.0) x)
          ((%fl=? x 0.0) (%fl/ 1.0 x))
          ((if (%fl<=? x 23.0) (%fl=? x (%flfloor x)) #f) ; exact factorials
           (let loop ((k 2.0) (p 1.0)) (if (%fl<? k x) (loop (%fl+ k 1.0) (%fl* p k)) p)))
          ((%fl>=? x 0.5) (if (%fl>? x 171.7) +inf.0 (fl-gamma+ x)))
          ((%fl=? x (%flfloor x)) +nan.0)
          ((%fl<? x -140.0) ; 1/Gamma(1 - x) without overflow: Gamma(w) = h * (h e^-w sqrt(2 pi) e^stirling)
           (let* ((w (%fl- 1.0 x)) (h (%flexpt w (%fl* 0.5 (%fl- w 0.5)))))
             (%fl/ (%fl/ (%fl/ 3.141592653589793 (fl-sinpi x)) h)
                   (%fl* (%fl* h (%flexp (%fl- 0.0 w))) (%fl* 2.5066282746310002 (%flexp (fl-stirling w)))))))
          (else (%fl/ 3.141592653589793 (%fl* (fl-sinpi x) (fl-gamma+ (%fl- 1.0 x)))))))
  ;; log Gamma(2 + z) = (1 - gamma) z + sum_k>=2 (-1)^k (zeta(k) - 1) z^k / k, |z| <= 1/2
  (define fl-zeta-1
    '#(0.6449340668482264 0.2020569031595943 0.08232323371113819 0.03692775514336993 0.01734306198444914
       0.008349277381922827 0.00407735619794434 0.0020083928260822143 0.0009945751278180853 0.0004941886041194645
       0.0002460865533080483 0.00012271334757848915 6.124813505870483e-05 3.058823630702049e-05 1.528225940865187e-05
       7.637197637899763e-06 3.81729326499984e-06 1.908212716553939e-06 9.539620338727962e-07 4.769329867878064e-07
       2.38450502727733e-07 1.1921992596531106e-07 5.960818905125948e-08 2.980350351465228e-08 1.4901554828365043e-08
       7.45071178983543e-09 3.725334024788457e-09 1.862659723513049e-09 9.313274324196682e-10 4.656629065033784e-10
       2.3283118336765053e-10 1.164155017270052e-10 5.820772087902701e-11 2.9103850444971e-11 1.4551921891041985e-11
       7.275959835057482e-12 3.637979547378651e-12 1.818989650307066e-12 9.094947840263888e-13 4.547473783042154e-13))
  (define (fl-lgamma2 z)
    (let loop ((k 41) (s 0.0))
      (if (%< k 2)
          (%fl* z (%fl+ 0.42278433509846713 (%fl* z s)))
          (let ((c (%fl/ (%vector-ref fl-zeta-1 (%- k 2)) (%inexact k))))
            (loop (%- k 1) (%fl+* s (%fl- 0.0 z) c))))))
  ;; log |Gamma(x)| for x > 0
  (define (fl-lgamma+ x)
    (cond ((%fl>=? x 12.0) (%fl+ (%fl- (%fl* (%fl- x 0.5) (%fllog x)) x) (%fl+ 0.9189385332046728 (fl-stirling x))))
          ((%fl<? (%flabs (%fl- x 2.0)) 0.5) (fl-lgamma2 (%fl- x 2.0)))
          ((%fl<? (%flabs (%fl- x 1.0)) 0.25) (let ((z (%fl- x 1.0))) (%fl- (fl-lgamma2 z) (fllog1+ z))))
          (else (%fllog (fl-gamma+ x)))))
  (define (flloggamma x)
    (cond ((if (%fl=? x x) #f #t) (%values x 1.0))
          ((%fl=? (%flabs x) +inf.0) (%values +inf.0 1.0))
          ((%fl>? x 0.0) (%values (if (%fl<? x 1e-300) (%fl- 0.0 (%fllog x)) (fl-lgamma+ x)) 1.0))
          ((%fl=? x (%flfloor x)) (%values +inf.0 1.0))
          (else
           (let ((s (fl-sinpi x)))
             (%values (%fl- (%fllog (%fl/ 3.141592653589793 (%flabs s))) (fl-lgamma+ (%fl- 1.0 x)))
                      (if (%fl<? s 0.0) -1.0 1.0))))))

  ;; ---- erf and erfc ----
  ;; exp(-x^2) with x^2 split exactly into hi + lo (x = xh + xl, xh 26 bits)
  (define (fl-expmx2 x)
    (let* ((t (%fl* x 134217729.0)) (xh (%fl- t (%fl- t x))) (xl (%fl- x xh)))
      (%fl* (%flexp (%fl- 0.0 (%fl* xh xh))) (%flexp (%fl- 0.0 (%fl* xl (%fl+ xl (%fl* 2.0 xh))))))))
  ;; erf(a), 0 <= a < 2.5: 2a/sqrt(pi) e^-a^2 sum (2a^2)^n / (1 3 ... (2n+1)), positive terms
  (define (fl-erf-series a)
    (let ((a2 (%fl* 2.0 (%fl* a a))))
      (let loop ((n 1) (t 1.0) (s 1.0))
        (let ((t (%fl/ (%fl* t a2) (%inexact (%+ (%* 2 n) 1)))))
          (if (%fl<? t (%fl* s 1e-17))
              (%fl* (%fl* 1.1283791670955126 a) (%fl* (fl-expmx2 a) s))
              (loop (%+ n 1) t (%fl+ s t)))))))
  ;; erfc(a), a >= 2: the even part of Laplace's continued fraction, evaluated backwards
  ;; erfc(a) = e^-a^2 / sqrt(pi) * 2a / (2a^2+1 - 1*2/(2a^2+5 - 3*4/(2a^2+9 - ...)))
  (define (fl-erfc-cf a)
    (if (%fl>? a 27.3)
        0.0
        (let* ((b (%fl+ (%fl* 2.0 (%fl* a a)) 1.0))
               (n (%+ 12 (%exact (%flfloor (%fl/ 1200.0 (%fl* a a)))))))
          (let loop ((k n) (f (%fl+ b (%fl* 4.0 (%inexact n)))))
            (if (%= k 0)
                (%fl* (%fl/ (%fl* 2.0 a) f) (%fl* 0.5641895835477563 (fl-expmx2 a)))
                (loop (%- k 1)
                      (%fl- (%fl+ b (%fl* 4.0 (%inexact (%- k 1))))
                            (%fl/ (%inexact (%* (%- (%* 2 k) 1) (%* 2 k))) f))))))))
  (define (flerf x)
    (let ((a (%flabs x)))
      (%flcopysign
       (cond ((%fl<? a 2.5) (fl-erf-series a))
             ((%fl<? a 6.0) (%fl- 1.0 (fl-erfc-cf a)))
             ((%fl=? a a) 1.0)
             (else a))
       x)))
  (define (flerfc x)
    (let ((a (%flabs x)))
      (cond ((if (%fl=? x x) #f #t) x)
            ((%fl<? x 0.0) (%fl+ 1.0 (if (%fl<? a 2.5) (fl-erf-series a) (%fl- 1.0 (fl-erfc-cf a)))))
            ((%fl<? a 0.5) (%fl- 1.0 (fl-erf-series a)))
            ((%fl<? a 2.0) (fl-erfc-mid a))
            (else (fl-erfc-cf a)))))
  ;; erfc on [1/2, 2): 1 - erf loses up to 8 bits there, so the continued fraction is used too, with
  ;; enough terms for its slower convergence
  (define (fl-erfc-mid a) (fl-erfc-cf a))

  ;; ---- Bessel functions of integer order ----
  ;; small x (< 1): the power series of J_n, Y_0 and Y_1
  (define (fl-jn-series n x)
    (let ((h (%fl* 0.5 x)))
      (let first ((k 1) (t 1.0))
        (if (%<= k n)
            (first (%+ k 1) (%fl/ (%fl* t h) (%inexact k)))
            (let ((q (%fl- 0.0 (%fl* h h))))
              (let loop ((k 1) (term t) (s t))
                (let ((term (%fl/ (%fl* term q) (%inexact (%* k (%+ k n))))))
                  (if (%fl<=? (%flabs term) (%fl* (%flabs s) 1e-17))
                      (%fl+ s term)
                      (loop (%+ k 1) term (%fl+ s term))))))))))
  (define (fl-y0-series x)
    (let ((q (%fl* 0.25 (%fl* x x))))
      (let loop ((k 1) (t 1.0) (h 0.0) (s 0.0))
        (let* ((t (%fl/ (%fl* t q) (%inexact (%* k k)))) (h (%fl+ h (%fl/ 1.0 (%inexact k))))
               (term (%fl* t h)) (s (if (%= (%remainder k 2) 1) (%fl+ s term) (%fl- s term))))
          (if (%fl<=? term (%fl* (%flabs s) 1e-17))
              (%fl* 0.6366197723675814 (%fl+ (%fl* (%fl+ (%fllog (%fl* 0.5 x)) 0.5772156649015329) (fl-jn-series 0 x)) s))
              (loop (%+ k 1) t h s))))))
  (define (fl-y1-series x)
    (let* ((hx (%fl* 0.5 x)) (q (%fl* hx hx)))
      (let loop ((k 0) (t hx) (hk 0.0) (s 0.0))
        (let* ((hk1 (%fl+ hk (%fl/ 1.0 (%inexact (%+ k 1)))))
               (term (%fl* t (%fl+ hk hk1)))
               (s (if (%= (%remainder k 2) 0) (%fl+ s term) (%fl- s term))))
          (if (if (%> k 0) (%fl<=? term (%fl* (%flabs s) 1e-17)) #f)
              (%fl+ (%fl- (%fl* (%fl* 0.6366197723675814 (%fl+ (%fllog hx) 0.5772156649015329)) (fl-jn-series 1 x))
                          (%fl/ 0.6366197723675814 x))
                    (%fl* -0.3183098861837907 s))
              (loop (%+ k 1) (%fl/ (%fl* t q) (%inexact (%* (%+ k 1) (%+ k 2)))) hk1 s))))))
  ;; Miller's backward recurrence from an even m well above max(n, x), normalised by
  ;; J_0 + 2 (J_2 + J_4 + ...) = 1; it also sums the Neumann series of Y_0 and Y_1:
  ;;   Y_0 = 2/pi ((log(x/2) + gamma) J_0 - 2 sum_k>=1 (-1)^k J_2k / k)
  ;;   Y_1 = 2/pi ((log(x/2) + gamma) J_1 - J_0 / x + sum_k>=1 (-1)^k (J_2k-1 - J_2k+1) / k)
  ;; Returns J_n (want 0), or Y_0 (want 1) or Y_1 (want 2).
  (define (fl-miller n x want)
    (let* ((top (if (%fl>? x (%inexact n)) (%exact (%flceiling x)) n))
           (m (%* 2 (%quotient (%+ (%+ top 24) (%exact (%flfloor (%flsqrt (%fl* 60.0 (%inexact top)))))) 2))))
      (let loop ((k m) (jk1 0.0) (jk 1.0) (jn 0.0) (norm 0.0) (s0 0.0) (s1 0.0) (j2 0.0))
        ;; invariants: jk = j_k, jk1 = j_k+1, j2 = j_k+2 (scaled alike)
        (let* ((norm (if (%= (%remainder k 2) 0) (%fl+ norm (if (%= k 0) jk (%fl* 2.0 jk))) norm))
               (jn (if (%= k n) jk jn))
               (s0 (if (if (%> k 0) (%= (%remainder k 2) 0) #f)
                       (let ((t (%fl/ jk (%inexact (%quotient k 2))))) (if (%= (%remainder k 4) 0) (%fl+ s0 t) (%fl- s0 t)))
                       s0))
               (s1 (if (%= (%remainder k 2) 1)
                       (let ((t (%fl/ (%fl- jk j2) (%inexact (%quotient (%+ k 1) 2)))))
                         (if (%= (%remainder (%quotient (%+ k 1) 2) 2) 0) (%fl+ s1 t) (%fl- s1 t)))
                       s1)))
          (if (%= k 0)
              (let ((j0 (%fl/ jk norm)) (j1 (%fl/ jk1 norm)) (l (%fl+ (%fllog (%fl* 0.5 x)) 0.5772156649015329)))
                (cond ((%= want 0) (%fl/ jn norm))
                      ((%= want 1) (%fl* 0.6366197723675814 (%fl- (%fl* l j0) (%fl* 2.0 (%fl/ s0 norm)))))
                      (else (%fl* 0.6366197723675814 (%fl+ (%fl- (%fl* l j1) (%fl/ j0 x)) (%fl/ s1 norm))))))
              (let ((jm (%fl- (%fl* (%fl/ (%inexact (%* 2 k)) x) jk) jk1)))
                (if (%fl>? (%flabs jm) 1e250) ; rescale everything
                    (loop (%- k 1) (%fl* jk 1e-250) (%fl* jm 1e-250) (%fl* jn 1e-250) (%fl* norm 1e-250)
                          (%fl* s0 1e-250) (%fl* s1 1e-250) (%fl* jk1 1e-250))
                    (loop (%- k 1) jk jm jn norm s0 s1 jk1))))))))
  ;; large x (>= 25): Hankel's asymptotic expansions of J_nu and Y_nu, nu = 0, 1
  (define (fl-hankel nu x)
    (let ((mu (%inexact (%* 4 (%* nu nu)))) (z (%fl/ 1.0 (%fl* 8.0 x))))
      (let loop ((k 1) (t 1.0) (p 1.0) (q 0.0))
        (let ((t2 (%fl/ (%fl* (%fl* t (%fl- mu (%inexact (%* (%- (%* 2 k) 1) (%- (%* 2 k) 1))))) z) (%inexact k))))
          (if (if (%fl<? (%flabs t2) 1e-18) #t (%> k 60))
              (let* ((c (%flcos x)) (s (%flsin x))
                     (cc (if (%= nu 0) (%fl* 0.7071067811865476 (%fl+ c s)) (%fl* 0.7071067811865476 (%fl- s c))))
                     (sc (if (%= nu 0) (%fl* 0.7071067811865476 (%fl- s c)) (%fl* -0.7071067811865476 (%fl+ s c))))
                     (f (%flsqrt (%fl/ 0.6366197723675814 x))))
                (%values (%fl* f (%fl- (%fl* p cc) (%fl* q sc))) (%fl* f (%fl+ (%fl* p sc) (%fl* q cc)))))
              (let ((r (%remainder k 4)))
                (loop (%+ k 1) t2
                      (if (%= r 2) (%fl- p t2) (if (%= r 0) (%fl+ p t2) p))
                      (if (%= r 1) (%fl+ q t2) (if (%= r 3) (%fl- q t2) q)))))))))
  (define (fl-int-arg who n)
    (if (%exact-integer? n) n (error (if (%eq? who 1) "flfirst-bessel: expected exact integer" "flsecond-bessel: expected exact integer") n)))
  ;; J_n, n >= 0, x > 0
  (define (fl-jn n x)
    (cond ((%fl<? x 1.0) (fl-jn-series n x))
          ((if (%fl<? x 25.0) #t (%fl>? (%inexact n) x)) (fl-miller n x 0))
          (else
           (call-with-hankel x
            (lambda (j0 j1 y0 y1)
              (let loop ((k 1) (a j0) (b j1))
                (if (%>= k n) (if (%= n 0) a b)
                    (loop (%+ k 1) b (%fl- (%fl* (%fl/ (%inexact (%* 2 k)) x) b) a)))))))))
  (define-syntax call-with-hankel
    (syntax-rules ()
      ((_ x f) (%call-with-values (lambda () (fl-hankel 0 x))
                 (lambda (j0 y0) (%call-with-values (lambda () (fl-hankel 1 x))
                                   (lambda (j1 y1) (f j0 j1 y0 y1))))))))
  (define (flfirst-bessel n x)
    (fl-int-arg 1 n)
    (cond ((if (%fl=? x x) #f #t) x)
          ((%< n 0) (let ((j (flfirst-bessel (%- 0 n) x))) (if (%= (%remainder n 2) 0) j (%fl- 0.0 j))))
          ((%fl<? x 0.0) (let ((j (flfirst-bessel n (%fl- 0.0 x)))) (if (%= (%remainder n 2) 0) j (%fl- 0.0 j))))
          ((%fl=? x 0.0) (if (%= n 0) 1.0 0.0))
          ((%fl=? x +inf.0) 0.0)
          (else (fl-jn n x))))
  (define (flsecond-bessel n x)
    (fl-int-arg 2 n)
    (cond ((if (%fl=? x x) #f #t) x)
          ((%< n 0) (let ((y (flsecond-bessel (%- 0 n) x))) (if (%= (%remainder n 2) 0) y (%fl- 0.0 y))))
          ((%fl<? x 0.0) +nan.0)
          ((%fl=? x 0.0) -inf.0)
          ((%fl=? x +inf.0) 0.0)
          (else
           (let-y01 x
            (lambda (y0 y1)
              (if (%= n 0) y0
                  (let loop ((k 1) (a y0) (b y1)) ; forward recurrence: stable for Y
                    (if (%>= k n) b
                        (loop (%+ k 1) b (%fl- (%fl* (%fl/ (%inexact (%* 2 k)) x) b) a))))))))))
  (define (let-y01 x f)
    (cond ((%fl<? x 1.0) (f (fl-y0-series x) (fl-y1-series x)))
          ((%fl<? x 25.0) (f (fl-miller 0 x 1) (fl-miller 0 x 2)))
          (else (call-with-hankel x (lambda (j0 j1 y0 y1) (f y0 y1)))))))
