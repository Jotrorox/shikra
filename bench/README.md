# bench/ -- r7rs-benchmarks harness

Measures Shikra against Chez Scheme and Gambit on ecraven's
[r7rs-benchmarks](https://github.com/ecraven/r7rs-benchmarks), the suite named by
milestone M4 in `docs/shikra-spec-0.1.tex` §13 ("*Done when* size targets are met
and the `r7rs-benchmarks` suite runs, with results recorded against Chez and
Gambit").

| File | Purpose |
|---|---|
| `fetch.sh` | clones the suite into `bench/r7rs-benchmarks/` at the pinned commit (not committed) |
| `run.sh` | builds and runs benchmarks for one implementation, writes `results/LABEL-YYYYMMDD-HHMM.csv` |
| `report.sh` | merges the newest CSV of each series into `RESULTS.md` (also `run.sh report`) |
| `shikra-unsupported.txt` | benchmarks Shikra cannot run by design (see below); `run.sh` records them as `unsupported` |
| `impl/` | preludes/postludes for Shikra and Chez, Gambit's unsafe prelude |
| `chez-r7rs/` | a small R7RS compatibility layer for Chez Scheme (see "Chez" below) |
| `lib/limit.pl` | portable wall-clock limit (macOS has no `timeout`), wraps `/usr/bin/time -l` |
| `lib/chez-compile.ss` | `compile-program` driver for Chez |
| `results/` | raw CSVs (committed) |
| `RESULTS.md` | the merged table |
| `BASELINE-FAILURES.md` | the pre-optimizer Shikra baseline's failures, grouped by cause |
| `work/` | per-run logs, assembled sources, binaries (ignored by git) |

## Re-running everything

```sh
sh bench/fetch.sh                                   # suite at the pinned commit
/opt/homebrew/bin/brew install chezscheme gambit-scheme   # chez, gsc (once)

bench/run.sh chez                                   # ~2 min
bench/run.sh gambit                                 # ~4 min (gsc compile ~2 s per benchmark)
bench/run.sh chez-unsafe                            # optional unsafe references
bench/run.sh gambit-unsafe

# a Shikra binary built outside the repo root (the root ./shikra is the default):
SHIKRA=/path/to/shikra bench/run.sh shikra
SHIKRA=/path/to/shikra bench/run.sh shikra-unsafe
SHIKRA=/path/to/shikra bench/run.sh shikra-interp   # optional, bytecode interpreter

sh bench/report.sh                                  # -> bench/RESULTS.md
```

A subset: `bench/run.sh shikra fib tak nboyer`. Options: `-t SECS` run limit
(env `TIMEOUT`, default 120), `-c SECS` compile limit (env `COMPILE_TIMEOUT`,
default 600), `-l LABEL` series name (env `LABEL`, default the implementation
name). `SHIKRA_FLAGS="--gc=bump"` adds flags to `shikra build`.

The pre-optimizer baseline is recorded under the labels `shikra-baseline` and
`shikra-baseline-unsafe` (`-l shikra-baseline`). `report.sh` uses the newest
`shikra`/`shikra-unsafe` CSVs for the Shikra columns and, once those exist, adds
the baseline as an extra column with a current/baseline ratio.

## What run.sh does

For each benchmark, exactly as upstream's `bench` script (`make_src_code`):

1. Assemble `prelude + src/NAME.scm + src/common.scm + postlude +
   src/common-postlude.scm` into `work/RUN/NAME.scm`. Gambit additionally gets
   upstream's `sed 's/^(import (scheme.*)$//'` (Gambit runs the program without
   R7RS imports, as upstream does).
2. Compile it, timed separately, under the compile limit:
   - `shikra`: `$SHIKRA build -O2 NAME.scm -o NAME` (`shikra-unsafe`: plus `--unsafe`;
     `shikra-interp`: no compile step, runs `$SHIKRA interp NAME.scm`);
   - `chez`: `compile-program` with `--optimize-level 2` (safe), the level upstream
     uses (`chez --optimize-level 2 --program`); `chez-unsafe`: `--optimize-level 3`.
     Upstream compiles at program load; compiling first only moves that time out of
     the run. Not a whole-program (`compile-whole-program`) build -- upstream does not
     use one either;
   - `gambit`: `gsc -o NAME -exe NAME.scm` with upstream's prelude
     `(declare (standard-bindings) (extended-bindings) (block))` (safe);
     `gambit-unsafe`: the same plus `(not safe)`.
3. Run it with `inputs/NAME.input` on stdin, the working directory being the run's
   work directory (which links `inputs/` and has an `outputs/` directory, so the
   relative paths in the inputs, e.g. `"inputs/bib"`, `"outputs/cat.output"`,
   work as in the upstream checkout), under the run limit, wrapped in
   `/usr/bin/time -l` for the peak RSS.
4. Record one CSV row: `benchmark,status,time_s,wall_s,compile_s,max_rss_kb,binary_bytes,note`.
   `time_s` is the benchmark's own timing (`+!CSVLINE!+` line from `common.scm`:
   jiffies around the measured loop only); `wall_s` is the whole process including
   start-up and reading the input; `binary_bytes` is the executable size (Shikra,
   Gambit). Status is one of `ok`, `compile-error`, `runtime-error` (non-zero exit
   or no result line), `wrong-result` (the benchmark's own check failed:
   `INCORRECT`), `timeout`, `unsupported`; `note` holds the first error line or the
   unsupported reason. Header lines `# key=value` record implementation version,
   flags, date, machine, suite commit and limits.

Benchmarks run one at a time. **Run limit: 120 s** instead of upstream's 300 s: the
slowest reference benchmark takes under 7 s (Chez) / 9 s (Gambit) here, so 120 s
still allows a ~20x slowdown everywhere while bounding a full run with many
pathological cases to well under two hours. Set `-t 300` for upstream's limit.
**Compile limit: 600 s** (Gambit needs ~2 s per benchmark; Shikra compiles in
well under a second today).

### Shikra prelude and postlude

`impl/shikra-prelude.scm` is a plain `(import (scheme base) (scheme write)
(scheme time))` -- the libraries `common.scm` needs (`display`, `write`,
`flush-output-port`, `inexact`, `round`, `current-second`, `current-jiffy`,
`jiffies-per-second`); each benchmark brings its own `(import ...)` line
(`(scheme base) (scheme read) (scheme write) (scheme time)` plus `cxr`, `char`,
`file`, `inexact`, `complex` as needed), which follows the prelude as a second
import declaration, as R7RS §5.1 allows. Chibi, Cyclone and Larceny need nothing
more than an implementation-name definition either. `impl/shikra-postlude.scm`
defines `this-scheme-implementation-name`.

### Chez

Chez Scheme 10.4.1 has no R7RS libraries. Upstream runs Chez with
`--libdirs /home/nex/scheme/chez`, a private directory not in the repository;
ecraven's public `chez-scheme-libraries` (2018) no longer compiles on Chez 10
(`vector-copy!`/`vector-append` now clash with Chez built-ins) and is GPL-3, so
`chez-r7rs/` is a fresh, minimal layer: it re-exports Chez's own bindings where
their semantics are R7RS (so Chez's optimizer sees its own primitives), and
defines only what is missing or different (R6RS argument order of `vector-copy!`,
`string-copy!`, `bytevector-copy!`; optional start/end arguments; Chez 10's
`(vector-copy v start count)`; three-argument `error`; `member`/`assoc` with a
predicate; R7RS `define-record-type`, expanded to Chez's native record definition;
`floor/` and friends; `cond-expand`; bytevector ports; `(scheme time)` on the
monotonic clock). The libraries are compiled once per optimize level into
`work/chez-r7rs-O2/` and `-O3/`. Chez top-level programs accept only one
`import` form, so `impl/chez-prelude.scm` imports Chez's `import` keyword and the
benchmark's own `(import ...)` line becomes a body-level import; nothing in the
benchmark source changes.

### Gambit

Gambit v4.9.8 from Homebrew (`gambit-scheme`, which compiles its C with `gcc-16`).
Settings are upstream's (`gambitc_comp`), including stripping the import line.

## Benchmark classification for Shikra

57 benchmarks (upstream's `all`). **4 cannot run by design** and are recorded as
`unsupported (reason)` for every Shikra series; **0 depend on tier 3
continuations**; **53 must run**.

### Unsupported by design

| Benchmark | Restriction | Evidence in the benchmark | Spec sentence |
|---|---|---|---|
| `pi` | bignums | Brent-Salamin/Borwein pi to 50-500 digits: `(expt 10 nb-digits)`, `exact-integer-sqrt` of 100+ digit integers; its input contains the 50-500 digit expected results, which `read` cannot represent | §3.2: "**Exact integers are 63-bit fixnums.** Overflow raises an error and never wraps. Optional bignums may come later, behind a flag." |
| `chudnovsky` | bignums | Chudnovsky pi to 50-1000 digits: `(expt 640320 3)` times `(expt b 3)`, `(expt 100 digits)`; 50-1000 digit literals in its input | §3.2, as above |
| `mbrotZ` | complex numbers | imports `(scheme complex)`; the inner loop is `make-rectangular`, `real-part`, `imag-part` on complex values | §3: "Complex numbers and `(scheme r5rs)` are omitted."; §3.1 table: "`(scheme complex)` Omitted -- no complex numbers" |
| `compiler` | complex numbers, bignums | imports `(scheme complex)` and calls `real-part`/`imag-part` (lines 7184-7187, 8416-8417); defines `flonum-sign-bit` as the literal `9223372036854775808` = 2^63 (line 8491), outside the 63-bit fixnum range | §3.1 table (`(scheme complex)` omitted); §3.2 (63-bit fixnums) |

No benchmark uses `eval`, `environment`, `load` or `(scheme r5rs)` (`peval`'s
and `scheme`'s `eval` are their own interpreters' procedures inside quoted data
or with other names), and none depends on exact rationals: the only `/` on exact
integers whose result feeds an exact-only operation is `bv2string`'s
`(* 4 (round (/ n 4)))` in `random-bytevector4`, which is defined but never
called; `mperm`'s `(/ (* n (+ n 1) (factorial n)) 2)` is an exact integer
(n = 10); `common.scm`'s `(inexact (/ jifs j/s))` is converted to a flonum
anyway, so Shikra's flonum `/` (§3.2: "`(/ 1 3)` returns the flonum 0.333...")
gives the same printed time.

### Continuations (tier 3)

Spec §8.2: tier 3 is when "the program stores `k`, returns it, or re-enters it
later"; compiled code today implements tiers 1 and 2 and raises "re-entrant
continuation not supported yet (milestone M5)" only when a continuation whose
frame is gone is invoked (DESIGN.md, "Tier 3 (M5)"). No benchmark re-enters a
continuation, so none is excluded:

| Benchmark | call/cc use | Tier |
|---|---|---|
| `ctak` | `k` passed down to `ctak-aux` and invoked while its frame is live (escape) | 2 |
| `fibc` | continuation passed to `addc`/`fibc` and invoked to return (escape) | 2 |
| `puzzle` | `return` from a `do` loop inside `trial` (escape) | 1/2 |
| `maze` | `quit` from a `vector-for-each-rev` callback (escape) | 2 |
| `quicksort` | `return` from the result check's `do` loop (escape) | 1 |
| `compiler` | stores `k` in the global `throw-to-exception-handler` and calls it only inside `with-exception-handling`'s extent (escape through a stored `k`; unsupported anyway, above) | 2 |
| `dynamic`, `scheme` | `call-with-current-continuation` appears only as data (a type environment, a commented-out primitive) | -- |

`ctak` and `fibc` are dominated by `call/cc` and measure the tier-2 capture cost
(a jump buffer per capture, `sigsetjmp` on Apple arm64; spec §8.2: "Costs a few
words per capture").

### Other notes on the benchmark sources (findings, not patched)

- `matrix` calls `div` (in `det-upper-bound`), an R6RS procedure that is not in
  R7RS-small; strictly R7RS implementations report an unbound identifier.
  `floor-quotient` would be the portable spelling. Chez and Gambit provide `div`.
- The file benchmarks (`cat`, `tail`, `wc`, `sum1`, `read1`, `parsing`,
  `dynamic`, `slatex`, `ray`) open files by relative path; `run.sh` runs them in a
  directory that mirrors the upstream layout.
- Gambit with `(declare (not safe))` crashes (SIGSEGV) on `graphs` and `matrix`;
  with safe declarations both run correctly. Unsafe Gambit code is undefined on
  any type error, so this is recorded as is, not investigated.
