;; Shikra prelude for the r7rs-benchmarks (bench/run.sh).
;; The libraries common.scm needs (run-r7rs-benchmark uses display/write,
;; flush-output-port, inexact, round, current-second, current-jiffy,
;; jiffies-per-second); each benchmark adds its own (import ...) after this one.
(import (scheme base) (scheme write) (scheme time))
