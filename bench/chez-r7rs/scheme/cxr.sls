;; (scheme cxr) for Chez Scheme 10 -- see base.sls.
(library (scheme cxr)
  (export caaaar caaadr caaar caadar caaddr caadr cadaar cadadr cadar caddar
          cadddr caddr cdaaar cdaadr cdaar cdadar cdaddr cdadr cddaar cddadr
          cddar cdddar cddddr cdddr)
  (import (chezscheme)))
