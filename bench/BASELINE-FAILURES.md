# Shikra baseline: failures and findings

Pre-optimizer baseline of the r7rs-benchmarks suite (`bench/run.sh`, labels
`shikra-baseline` and `shikra-baseline-unsafe`), for the compiler engineers.

- **Compiler:** commit `986aae1` ("(scheme char): 22 procedures as leaf
  primitives ..."), built outside the repo with `cc -std=c99 -O2 src/*.c -o shikra`
  (Apple clang, macOS 27.3, Apple M5).
- **Flags:** `shikra build -O2` (safe) and `shikra build -O2 --unsafe`.
- **CSVs:** `results/shikra-baseline-20261119-0708.csv`,
  `results/shikra-baseline-unsafe-20261119-0711.csv`.

## Result

| | safe | `--unsafe` |
|---|---|---|
| ok | **53** | **53** |
| compile-error / runtime-error / wrong-result / timeout | 0 | 0 |
| unsupported by design (`pi`, `chudnovsky`, `mbrotZ`, `compiler`; see README.md) | 4 | 4 |

Every benchmark that Shikra's restrictions allow builds, runs and passes the
benchmark's own result check, in both modes. Compile time is 0.4-1.4 s per
benchmark (32 s for all 53). The four unsupported benchmarks were also built
once by hand to confirm that they fail for the documented reason and no other:

| Benchmark | Phase | Message |
|---|---|---|
| `pi` | run | `Error: number cannot be represented (no bignums or rationals)` (reading the 50+ digit input), exit 70 |
| `chudnovsky` | run | same message, exit 70 |
| `mbrotZ` | compile | `mbrotZ.scm:9:23: error: library (scheme complex) not found` |
| `compiler` | compile | `compiler.scm:8496:25: error: number cannot be represented (no bignums or rationals)` (the literal `9223372036854775808`; it also imports `(scheme complex)`) |

## Failures seen on the way (resolved)

First attempt, commit `e730805` (`results/shikra-baseline-20261119-0701.csv`,
`...-unsafe-20261119-0705.csv`): 50 ok, 3 compile errors, all one cause:

| Benchmarks | Phase | Message | Cause |
|---|---|---|---|
| `parsing`, `scheme`, `slatex` | compile | `parsing.scm:38:23: error: library (scheme char) not found` (likewise `scheme.scm:10:53`, `slatex.scm:12:23`) | `(scheme char)` not yet on `main`; it landed in `986aae1` and all three pass there |

Before that (commit `143ed71`, 06:31) nothing compiled:
`error: library (scheme time) not found` for every benchmark.

## Findings for the optimizer work (not failures)

Ratios are Shikra safe time / reference time from `RESULTS.md`
(geometric mean over the 53: **1.36x Chez**, **1.24x Gambit**).

1. **`--unsafe` removes almost nothing.** (Resolved in M4-2: `RT_UNSAFE` removes the type, bounds,
   overflow, undefined-variable, not-a-procedure, arity and literal checks; unsafe/safe geometric
   mean 0.88 in the m4-2 series.) Geometric mean of unsafe/safe time is
   1.04 (range 0.83-1.28, i.e. noise). `shikra emit-c --unsafe` differs from the
   safe C only by `#define RT_UNSAFE 1`, and in `src/rt.h` that macro only
   disables `RT_CHECK_MUTABLE_PAIR` (the literal-pair check in
   `set-car!`/`set-cdr!`). `RT_CHECK` type checks (e.g. `rt_car`) and the
   `rt_arity_error` argument-count checks (45 of them in `fib`'s C) stay in, unlike
   spec §3.3 ("With
   `--unsafe` the remaining checks are removed"). Expected at this stage (no
   optimizer yet), but it means the `Shikra unsafe` column measures the same code
   as the safe one for now.
2. **The collector lets the heap grow far beyond the live data.** (M4-3: collections got 10-30%
   cheaper -- O(1) block lookup, lazy object-start walk -- but the RSS figures stand; graphs is
   bounded by fragmentation a non-moving collector keeps: DESIGN.md "M4-3 as built", Collector.) Peak RSS
   (MB, Shikra / Chez / Gambit): `graphs` 1062 / 84 / 112, `earley`
   2926 / 1208 / 2877, `nboyer` 532 / 471 / 1201, `sboyer` 267 / 147 / 307,
   `gcbench` 283 / 185 / 353. On `graphs` (allocates ~7 GB in total) the
   collector is most of the run time: `--gc=bump` (no collection) runs it in
   4.42 s versus 11.16 s with the default `immix`, so collection costs ~60% of the
   time, and `graphs` is the worst ratio of the suite (6.1x Chez).
3. **Tier-2 `call/cc` capture cost.** `ctak` 4.8x Chez and `fibc` 4.2x Chez
   (1.2x and 1.1x Gambit): every capture goes through `rt_call_cc` with a jump
   buffer (`sigsetjmp` on Apple arm64). The continuations never leave their
   procedure's dynamic extent in a way that would need more than a non-local
   return, so tier-1/2 classification at compile time is where these gain.
4. **Generic arithmetic through calls.** Even trivial code calls the library's
   variadic arithmetic out of line, e.g. `fib`'s body is
   `P_scheme_base__lt__1(...)`, `P_scheme_base__minus__2(...)`,
   `P_scheme_base__plus__3(...)`; the C compiler recovers part of this by
   inlining within the single translation unit (fib is 0.9x Chez), but not across
   deeper call chains: `ack` 5.3x Chez / 4.3x Gambit, `primes` 2.7x / 2.5x,
   `wc` 2.6x / 2.4x, `cpstak` 2.7x / 1.3x, `divrec` 2.1x / 1.5x.
5. **Floats.** `ray` 2.5x Chez / 3.8x Gambit, `simplex` 2.3x / 3.3x, `nucleic`
   2.3x / 3.0x, `sum1` 2.2x / 3.4x, `fibfp` 1.4x / 3.0x, `mbrot` 1.2x / 3.2x --
   boxed flonums, the case M4's flonum unboxing addresses.

Five worst ratios versus Chez (safe): `graphs` 6.13, `ack` 5.31, `ctak` 4.76,
`fibc` 4.23, `gcbench` 3.13. Best: `takl` 0.48, `string` 0.54, `ntakl` 0.57,
`mperm` 0.68, `bv2string` 0.72.
