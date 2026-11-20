# Binary sizes against the spec's targets (M4-3)

Spec §9.3, macOS arm64 column: hello world "about 50 KB (format floor)"; a program of
"1,000 to 2,000 lines: 100 to 200 KB". (The Linux "under 30 KB static" target belongs to the
Linux port.) Measured on macOS 27.3 arm64, Apple clang 21, `shikra build -O2` / `-Os`, bytes of
the executable as written (symbols kept: `shikra size` needs them; see below).

Lines are the benchmark's own source (`wc -l bench/r7rs-benchmarks/src/NAME.scm`); the program
built is the benchmark with the harness's prelude, `common.scm` and postlude (about 60 more
lines), as `bench/run.sh` assembles it. `r7rs/*` are the adapted Chibi test files of
`tests/r7rs/` (plus their 300-line `chibi/test.sld` library). "Before" is the compiler at the
start of M4-3 (commit 61d8207), "after" the M4-3 compiler.

| Program | Lines | -O2 before | -O2 after | -Os before | -Os after | 100-200 KB? |
|---|---|---|---|---|---|---|
| hello world (`tests/cases/hello.scm`) | 3 | 50,888 | 50,888 | 50,904 | 50,904 | about 50 KB: yes (pinned at 52,000 by `tests/run.sh size`) |
| r7rs/char | 118 | 122,744 | 114,120 | 104,664 | 99,128 | (below range) |
| r7rs/inexact | 122 | 118,240 | 110,976 | 100,448 | 95,376 | (below range) |
| r7rs/write | 201 | 139,784 | 144,824 | 120,152 | 112,344 | (below range) |
| r7rs/read | 249 | 247,128 | 232,456 | 195,096 | 181,416 | (below range) |
| conform | 469 | 114,328 | 105,160 | 114,184 | 107,624 | (below range) |
| graphs | 610 | 108,904 | 104,648 | 92,760 | 89,400 | (below range) |
| peval | 643 | 136,744 | 123,000 | 121,048 | 108,264 | (below range) |
| earley | 657 | 133,544 | 121,656 | 113,416 | 106,104 | (below range) |
| maze | 750 | 133,016 | 137,912 | 110,120 | 105,864 | (below range) |
| nboyer | 772 | 147,992 | 138,520 | 131,912 | 122,904 | (below range) |
| sboyer | 785 | 147,992 | 138,520 | 131,912 | 122,904 | (below range) |
| matrix | 805 | 127,096 | 122,088 | 110,568 | 107,048 | (below range) |
| parsing | 936 | 128,376 | 121,752 | 108,760 | 105,896 | yes |
| scheme | 1,055 | 293,960 | **241,352** | 214,632 | 175,672 | -Os yes, -O2 no |
| r7rs/base | 1,605 | 588,280 | **538,552** | 517,064 | **475,736** | no (see below) |
| dynamic | 2,328 | 350,280 | **310,808** | 198,184 | 181,448 | -Os yes, -O2 no |
| slatex | 2,354 | 225,944 | 175,176 | 175,416 | 143,336 | yes |
| nucleic | 3,485 | 436,904 | 239,048 | 420,120 | 222,024 | (above range) |

Mach-O sizes move in steps: each segment is padded to 16 KB, so a few hundred bytes of code
can cost or save a page (maze and r7rs/write at -O2 grew by one page while their code shrank
slightly elsewhere).

**Targets.** Hello world meets "about 50 KB" (the floor: four 16 KB segments, `__text` is 1.5 KB).
Of the programs in or near the 1,000-2,000 line range, parsing and slatex are inside 100-200 KB
at both levels, scheme and dynamic at `-Os` (176 KB and 181 KB) but not at `-O2` (241 KB, 311 KB:
the `-O2` inliner may grow the program by its own size plus 20,000 nodes, and both programs are
large interpreters full of small procedures it copies; the `-O2` budget was left alone because
shrinking it measured slower: below). r7rs/base misses at both levels: it is not a typical
program but 658 test cases, each a pair of closures plus its quoted expression for the report
(1,600 small procedures and 83 KB of literal pairs), and 26% of it is symbol table.

## What `shikra size` showed, and what was removed

`shikra size scheme.O2` before the changes: 294 KB = 90 KB program code, 28 KB `(scheme base)`
(procedures used as first-class values by the interpreter), 43 KB runtime (reader 9 KB,
collector 7 KB, printer 6 KB, float printing 6 KB), 11 KB literals, **77 KB symbol and string
tables**, 26 KB headers and padding. 2,093 symbols, of which 1,049 were `NAME.cold.N`: Apple
clang splits every cold path (each call of an error function) out into a function of its own,
with a symbol, a frame set-up and a branch (604 such fragments in the program body alone). And
every literal was its own `static const` object with its own symbol (383 in scheme, 1,312 in
r7rs/base, about 1,000 flonums in nucleic).

1. **No cold-path splitting.** The driver passes `-Xclang -fno-split-cold-code` when the C
   compiler is Apple's `cc` (or a `clang`): scheme 294 -> 250 KB, dynamic 350 -> 318 KB,
   r7rs/base 588 -> 568 KB. Speed unchanged within noise on nboyer, earley, paraffins, graphs,
   fib, tak, deriv, browse, puzzle, nqueens and string (min of 3, interleaved).
2. **One literal object.** All literals are members of one `static const struct rt_lits`
   (one symbol), and equal strings, flonums (same bits) and bytevectors share one member
   whichever library they came from (literals are read-only; R7RS lets equal constants be the
   same object). nucleic 422 -> 239 KB (its tables repeat the same flonums), scheme 250 ->
   241 KB, r7rs/base 568 -> 539 KB. (The sharing is also what makes the expansion cache's
   output identical to a fresh expansion's: DESIGN.md "M4-3 as built".)
3. Tried and **not** kept: a smaller `-O2` inlining budget (growth limited to a quarter or half
   of the program instead of all of it plus 20,000 nodes) takes dynamic to 213 KB and scheme to
   224 KB, but measured 8-20% slower on maze, nucleic and slatex (`-O2` must not trade speed);
   `-Wl,-no_data_const` saves a whole 16 KB segment on every macOS binary (hello world 34 KB) but
   makes the literals' page writable, against spec §3.2 ("Shikra places them in read-only
   data"; it only matters with `--unsafe`), so it was not adopted either.

Not avoidable weight, per `shikra size`: the reader in benchmarks that `read` their input
(9 KB), float printing in any program that prints a flonum (common.scm prints the time: 3-6 KB),
the collector (7-8 KB) in every allocating program, and on macOS about 26-38 KB of headers and
segment padding.

## Symbols

`shikra build` keeps the local symbols (the linker's default; nothing is stripped) because
`shikra size` attributes bytes through them. They are 10-26% of a binary (scheme 33 KB of 241 KB
after the changes above, r7rs/base 155 KB); `strip app` removes them, after which `shikra size`
refuses the binary ("has no Shikra symbols: it was stripped, or it was not built by shikra
build"). `shikra size` prints the symbol table's own share as a separate line, so the cost of
keeping it is visible.

## Linux (static, no libc), merged tree (integration)

Spec §9.3, Linux x86-64 column: hello world "under 30 KB, statically linked"; "1,000 to 2,000
lines: 60 to 150 KB". Measured in the `tests/linux.sh` containers (Debian trixie: GCC 14.2,
clang 19.1.7, GNU ld 2.44; arm64 native, x86-64 under qemu-user -- emulation does not change the
output), `shikra build -O2` / `-Os`, the same assembled benchmark programs as above; bytes as
written / after `strip` (symbols are kept by default for `shikra size`). KB = 1,000 bytes, the
range check is on the file as written. macOS (Apple clang, from the table above) for comparison.

| Program | Lines | Compiler | x86-64 -O2 | x86-64 -Os | arm64 -O2 | arm64 -Os | macOS -O2 / -Os |
|---|---|---|---|---|---|---|---|
| hello world | 3 | GCC | 3,248 / 2,088 | 3,024 / 1,864 | 4,672 / 2,488 | 4,168 / 2,056 | 50,888 / 50,904 |
| | | clang | 6,408 / 4,936 | 6,440 / 4,936 | 7,608 / 4,936 | 7,664 / 4,936 | |
| parsing | 936 | GCC | 75,736 / 68,624 | 52,416 / 44,560 | 82,536 / 69,712 | 61,016 / 47,680 | 121,760 / 105,904 |
| | | clang | 84,976 / 78,688 | 62,568 / 55,488 | 81,448 / 70,680 | 66,288 / 54,112 | |
| scheme | 1,055 | GCC | 201,512 / 170,736 | 119,696 / 88,016 | 233,824 / 182,576 | 144,784 / 95,888 | 241,360 / 175,680 |
| | | clang | 237,920 / 208,416 | 145,544 / 115,568 | 241,752 / 196,640 | 169,368 / 123,040 | |
| dynamic | 2,328 | GCC | 192,664 / 177,008 | 93,584 / 74,768 | 186,488 / 160,608 | 108,576 / 78,688 | 310,816 / 181,456 |
| | | clang | 312,464 / 297,552 | 149,920 / 131,936 | 288,368 / 264,520 | 154,744 / 126,072 | |
| slatex | 2,354 | GCC | 128,760 / 116,592 | 88,320 / 74,608 | 139,240 / 118,032 | 100,592 / 78,688 | 175,184 / 143,344 |
| | | clang | 163,128 / 152,416 | 112,520 / 100,528 | 159,888 / 142,592 | 122,680 / 103,264 | |

**Verdicts** (60-150 KB, file as written):
- Hello world: 3.0-7.7 KB everywhere, under 30 KB (pinned by `tests/run.sh shape` on Linux).
- `-Os`: inside the range on x86-64 with both compilers (clang's dynamic just: 149,920); parsing
  with GCC is below it (52 KB). On arm64 GCC is inside for all four; clang's scheme (169 KB) and
  dynamic (155 KB) are above.
- `-O2`: parsing is inside everywhere, slatex inside with GCC (129 / 139 KB) and above with clang
  (159-163 KB); scheme and dynamic are above with both compilers on both architectures (186-312 KB):
  the `-O2` inlining budget, as on macOS (where they are 241 and 311 KB). Stripping does not bring
  them into the range either.

**Size flags on Linux.** M4-3's size finding for macOS was Apple clang's cold-path splitting
(`-Xclang -fno-split-cold-code`). Neither Linux compiler needs an equivalent: upstream clang 19
rejects that option (the driver passed it to every `clang` before the integration, which broke
every clang build on Linux) and does not split cold code by default (0 `.cold` symbols in scheme
-O2); GCC 14's `-freorder-blocks-and-partition` produced no `.cold` parts here either (no unwind
tables), and `-fno-reorder-blocks-and-partition` leaves scheme, dynamic and slatex byte-identical.
So the flag is now passed only to Apple clang, and the Linux build line is unchanged. The literal
sharing (one `rt_lits` struct) applies on every platform. A program that uses `define-c` is a
hosted build (glibc, dynamic): hello world built with `--libc` is 17 KB on x86-64 and 72 KB on arm64 (GNU ld's 64 KB
page alignment of the separate segments) -- the static sizes above are for programs without FFI.
