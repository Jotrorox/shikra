;; Feature identifiers (spec §3.4): cond-expand sees r7rs, shikra, ieee-float, the operating
;; system and the architecture, and full-unicode only with --unicode=full (this file, built with it; features.scm without);
;; never ratios or exact-complex. (features) agrees with cond-expand.
(import (scheme base) (scheme write))
(define (has? f) (if (memq f (features)) #t #f))
(write (list (cond-expand (r7rs 'r7rs) (else 'no-r7rs))
             (cond-expand (shikra 'shikra) (else 'no-shikra))
             (cond-expand (ieee-float 'ieee-float) (else 'no-ieee-float))
             (cond-expand ((or darwin linux) 'os) (else 'no-os))
             (cond-expand ((or arm64 aarch64 x86-64) 'arch) (else 'no-arch))
             (cond-expand (full-unicode 'full-unicode) (else 'ascii))
             (cond-expand (ratios 'ratios) (else 'no-ratios))
             (cond-expand (exact-complex 'exact-complex) (else 'no-exact-complex))))
(newline)
(write (list (has? 'r7rs) (has? 'shikra) (has? 'ieee-float) (or (has? 'darwin) (has? 'linux))
             (or (has? 'arm64) (has? 'aarch64) (has? 'x86-64))
             (has? 'full-unicode) (has? 'ratios) (has? 'exact-complex)))
(newline)
