;; (shikra flonum) -- SRFI 144 (spec §10). The arithmetic, comparisons, rounding, the platform's
;; elementary functions and the bit-level operations are primitives (rt_ext.c; unboxed doubles in
;; compiled code); everything else is Scheme on top of them: flonum/core.scm (constants,
;; predicates, variadic folds, small procedures) and flonum/math.scm (cbrt, hypot, expm1, log1p,
;; log2/log10, the hyperbolic functions and their inverses, gamma, log-gamma, erf, erfc, Bessel
;; functions of integer order). Algorithms and measured accuracy: DESIGN.md "Extensions as built".
(define-library (shikra flonum)
  (import (shikra primitives) (only (scheme base) error numerator denominator))
  (export fl-e fl-1/e fl-e-2 fl-e-pi/4 fl-log2-e fl-log10-e fl-log-2 fl-1/log-2 fl-log-3 fl-log-pi
          fl-log-10 fl-1/log-10 fl-pi fl-1/pi fl-2pi fl-pi/2 fl-pi/4 fl-pi-squared fl-degree fl-2/pi
          fl-2/sqrt-pi fl-sqrt-pi fl-sqrt-2 fl-sqrt-3 fl-sqrt-5 fl-sqrt-10 fl-1/sqrt-2 fl-cbrt-2
          fl-cbrt-3 fl-4thrt-2 fl-phi fl-log-phi fl-1/log-phi fl-euler fl-e-euler fl-sin-1 fl-cos-1
          fl-gamma-1/2 fl-gamma-1/3 fl-gamma-2/3 fl-greatest fl-least fl-epsilon fl-fast-fl+*
          fl-integer-exponent-zero fl-integer-exponent-nan
          flonum fladjacent flcopysign make-flonum
          flinteger-fraction flexponent flinteger-exponent flnormalized-fraction-exponent flsign-bit
          flonum? fl=? fl<? fl>? fl<=? fl>=? flunordered? flinteger? flzero? flpositive? flnegative?
          flodd? fleven? flfinite? flinfinite? flnan? flnormalized? fldenormalized?
          flmax flmin fl+ fl* fl+* fl- fl/ flabs flabsdiff flposdiff flsgn flnumerator fldenominator
          flfloor flceiling flround fltruncate
          flexp flexp2 flexp-1 flsquare flsqrt flcbrt flhypot flexpt fllog fllog1+ fllog2 fllog10
          make-fllog-base
          flsin flcos fltan flasin flacos flatan flsinh flcosh fltanh flasinh flacosh flatanh
          flquotient flremainder flremquo
          flgamma flloggamma flfirst-bessel flsecond-bessel flerf flerfc)
  (include "flonum/core.scm" "flonum/math.scm"))
