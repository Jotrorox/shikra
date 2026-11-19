;; Chez prelude for the r7rs-benchmarks (bench/run.sh).
;; Chez's top-level programs take exactly one import form, so this one imports
;; Chez's own `import` keyword and the benchmark's (import (scheme ...)) line
;; that follows becomes a body-level import of the R7RS libraries in
;; bench/chez-r7rs.  this-scheme-implementation-name is in chez-postlude.scm.
(import (only (chezscheme) import machine-type scheme-version-number))
