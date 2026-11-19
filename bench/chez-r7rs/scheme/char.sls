;; (scheme char) for Chez Scheme 10 -- see base.sls.
(library (scheme char)
  (export char-alphabetic? char-ci<=? char-ci<? char-ci=? char-ci>=? char-ci>?
          char-downcase char-foldcase char-lower-case? char-numeric? char-upcase
          char-upper-case? char-whitespace? digit-value string-ci<=? string-ci<?
          string-ci=? string-ci>=? string-ci>? string-downcase string-foldcase
          string-upcase)
  (import (chezscheme))
  ;; ASCII digits only (Unicode Nd digits other than 0-9 return #f).
  (define (digit-value c)
    (and (char<=? #\0 c #\9) (- (char->integer c) (char->integer #\0)))))
