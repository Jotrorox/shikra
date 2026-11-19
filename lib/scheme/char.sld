;; (scheme char) -- R7RS 6.6, 6.7. The classes and case mappings are leaf primitives (rt_str.c):
;; ASCII rules by default, the Unicode tables of rt_unicode.c with --unicode=full (spec §3.1),
;; when cond-expand also sees full-unicode. string-upcase/-downcase/-foldcase use the full
;; mappings there (ß -> SS, final sigma); the -ci comparisons compare case-folded characters
;; and strings.
(define-library (scheme char)
  (import (shikra primitives))
  (export char-alphabetic? char-ci<=? char-ci<? char-ci=? char-ci>=? char-ci>? char-downcase
          char-foldcase char-lower-case? char-numeric? char-upcase char-upper-case?
          char-whitespace? digit-value string-ci<=? string-ci<? string-ci=? string-ci>=?
          string-ci>? string-downcase string-foldcase string-upcase)
  (begin
    (define (char-alphabetic? c) (%char-alphabetic? c))
    (define (char-numeric? c) (%char-numeric? c))
    (define (char-whitespace? c) (%char-whitespace? c))
    (define (char-upper-case? c) (%char-upper-case? c))
    (define (char-lower-case? c) (%char-lower-case? c))
    (define (digit-value c) (%digit-value c))
    (define (char-upcase c) (%char-upcase c))
    (define (char-downcase c) (%char-downcase c))
    (define (char-foldcase c) (%char-foldcase c))
    (define (string-upcase s) (%string-upcase s))
    (define (string-downcase s) (%string-downcase s))
    (define (string-foldcase s) (%string-foldcase s))
    (define-syntax define-ci-comparison
      (syntax-rules ()
        ((_ name prim key)
         (define (name a b . rest)
           (let loop ((x (key a)) (y (key b)) (rest rest))
             (if (prim x y)
                 (if (%null? rest) #t (loop y (key (%car rest)) (%cdr rest)))
                 #f))))))
    (define (char-key c) (%char->integer (%char-foldcase c)))
    (define (string-key s) (%string-foldcase s))
    (define (string=0? a b) (%= (%string-compare a b) 0))
    (define (string<0? a b) (%< (%string-compare a b) 0))
    (define (string>0? a b) (%> (%string-compare a b) 0))
    (define (string<=0? a b) (%<= (%string-compare a b) 0))
    (define (string>=0? a b) (%>= (%string-compare a b) 0))
    (define-ci-comparison char-ci=? %= char-key)
    (define-ci-comparison char-ci<? %< char-key)
    (define-ci-comparison char-ci>? %> char-key)
    (define-ci-comparison char-ci<=? %<= char-key)
    (define-ci-comparison char-ci>=? %>= char-key)
    (define-ci-comparison string-ci=? string=0? string-key)
    (define-ci-comparison string-ci<? string<0? string-key)
    (define-ci-comparison string-ci>? string>0? string-key)
    (define-ci-comparison string-ci<=? string<=0? string-key)
    (define-ci-comparison string-ci>=? string>=0? string-key)))
