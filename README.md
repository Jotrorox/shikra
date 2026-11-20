# Shikra

Shikra is an ahead-of-time compiler for R7RS-small Scheme. It compiles a whole program,
with every library it imports, into one C99 file and hands that to the system C compiler,
so the result is a small native executable with no interpreter inside. A bytecode
interpreter in the same tool provides a REPL. The specification is
[`docs/shikra-spec-0.1.tex`](docs/shikra-spec-0.1.tex); the architecture and what has been
built are in [`DESIGN.md`](DESIGN.md).

## Build

One command, no configure step, nothing but a C99 compiler:

```sh
cc -std=c99 -O2 -Wall -Wextra src/*.c -o shikra
```

(`make` does the same and also regenerates the committed `src/embed.c` after changes to
`lib/` or the runtime.) On Linux no `-lm` is needed: the runtime brings its own math
functions there. The tool uses `dlsym` (for `define-c` in the REPL); on glibc 2.34 and later
that is part of libc, so the one-liner is complete; on an older glibc add `-ldl`.

## Commands

```sh
shikra build main.scm -o app     # compile to a native executable (-O2 by default)
shikra run main.scm arg1 arg2    # build at -O0 into a cache directory and run it
shikra repl [main.scm]           # interactive interpreter; with a file, load it first
shikra check main.scm            # expand and analyse only; report errors and warnings
shikra emit-c main.scm > app.c   # print the generated C
shikra size app                  # break a binary down by library and procedure
```

Common flags: `-O0 | -O2 | -Os`, `--unsafe`, `--gc=immix|bump`, `--unicode=ascii|full`,
`-I dir`, `-g`, `--cc CC` (default `$SHIKRA_CC`, else `cc`), `--target TRIPLE`, `--link FILE`,
`-l NAME`, `--libc`, `--no-cache`, `-w`. `shikra --help` lists them.

**`shikra size app`** breaks an executable (Mach-O or ELF, also cross-built ones) down by
library, procedure and runtime component, so growth can be traced to its cause; the first line
also says how it is linked (static without libc, or hosted). It needs the symbols that
`shikra build` keeps, so it refuses a stripped binary. `--all` lists every procedure, `--tsv`
prints machine-readable rows.

**Expansion cache.** Expanded libraries are cached in `$SHIKRA_CACHE_DIR` (default
`$XDG_CACHE_HOME/shikra`, else `~/.cache/shikra`), keyed by their sources, their imports, the
compiler build and the target; an edited library or included file invalidates its entry.
`shikra run` reuses its executable while nothing changed, so a warm `shikra run` starts in
milliseconds. `--no-cache` disables the cache.

**`-g`** adds the source location of the failing expression to run-time errors
(`main.scm:12:5: error: car: expected pair 5`), also inside inlined library code, and passes
`-g` to the C compiler (the generated C is kept in the cache directory for the debugger).
Without `-g` no location code or string is in the binary.

## Extensions

Five small libraries for hot paths and C, standard programs never need them (spec §10):
`(shikra fixnum)` (SRFI 143), `(shikra flonum)` (SRFI 144, own math functions),
`(shikra f64vector)` (SRFI 4 `f64` vectors of raw doubles), `(shikra assume)` (SRFI 145:
checked, trusted with `--unsafe`), and `(shikra ffi)` with `define-c`:

```scheme
(import (scheme base) (shikra ffi))
(define-c c-hypot "hypot" (double double) double)
```

Types: `int long double float bool char string pointer void`. Functions of your own C files
are linked with `--link file.c` (or `.o`, `.a`) and libraries with `-l NAME`. The REPL calls C
functions of its own process (the C library, libm) with up to 6 integer-class and 8 `double`
arguments; everything else needs `shikra build`.

**Build modes.** A program is *static without libc* on Linux x86-64/arm64 unless it needs the
C library: when it contains a `define-c` binding (after tree shaking), or `--link`, `-l` or
`--libc` is given, it is a *hosted* build -- linked against the C library (and libm), dynamically
by default, like an ordinary C program. `shikra build` prints nothing about it; `shikra size`
shows the mode on its first line and `shikra emit-c` in a `build mode` comment. `--libc` forces
a hosted build. On macOS every build is hosted (libSystem).

**Bignums.** By default exact integers are 63-bit fixnums and overflow raises an error (spec §3.2).
`--bignum` gives exact integers of any size (`(expt 3 10000)`, factorials, reading and printing
long numbers in radix 2, 8, 10 and 16) and the `cond-expand` feature `bignum`; results that fit are
always fixnums again, and there are still no exact rationals (`(/ 1 3)` is a flonum). Only programs
built with the flag contain bignum code; see DESIGN.md "Bignums as built".

```sh
shikra run --bignum pi.scm        # also: build, check, emit-c, repl
```

## Platforms

| Target | Executables | C compilers tested | Test suite |
|---|---|---|---|
| macOS arm64 | dynamically linked against libSystem (the platform requires it) | Apple clang (default), GCC 16 (`tests/run.sh gcc`) | `make test` |
| Linux x86-64 | static, no libc (own `_start` and system calls); hosted (glibc, dynamic) with `define-c`, `--link`, `-l`, `--libc` | GCC 14, clang 19 | `tests/linux.sh amd64` (emulated) |
| Linux arm64 | the same | GCC 14, clang 19 | `tests/linux.sh arm64` |

Hello world is about 4–8 KB on Linux (spec target: under 30 KB) and about 50 KB on macOS.
The `shikra` tool itself is an ordinary libc program everywhere. Tier-3 (re-entrant)
continuations copy the C stack, so they need a process without hardware shadow-stack
enforcement (Intel CET, Arm GCS).

**Cross-compiling.** `--target` takes the usual triple spellings (`x86_64-linux-gnu`,
`x86_64-unknown-linux-musl`, `aarch64-linux`, `arm64-apple-darwin`, ...) and sets the runtime
variant, the `cond-expand` features and the link flags. The Linux runtime needs no headers
and no libraries, so any compiler that can emit code for the target works:

```sh
shikra build --target x86_64-linux --cc "zig cc" main.scm -o app-x64
shikra build --target aarch64-linux --cc clang main.scm -o app-arm64   # clang + ld.lld in PATH
```

Without `--cc`, `cc --target=...` is tried (on macOS with `-fuse-ld=lld`). A hosted build
(`define-c`) for another OS needs that target's C library too: `zig cc` brings one (it then
links against glibc dynamically); plain clang needs a sysroot.

## Tests

```sh
make test                     # everything (macOS: about 10 minutes)
sh tests/run.sh MODE          # one part: reader unit amalg expand compiled compiled2 r7rs
                              # levels interp stress tramp shape size cache debug driver gcc
                              # (see the script's header; `all` = everything but gcc)
sh tests/linux.sh arm64       # the suite on Linux in a podman container (native)
sh tests/linux.sh amd64 MODE  # x86-64, emulated with qemu-user (slow)
```

`tests/linux.sh` copies the working tree into a Debian container, builds the tool with the
one-liner using GCC and clang, and runs `tests/run.sh` once per program compiler. Under
emulation the collector stress passes are run with `LINUX_ENV=STRESS_SCALE=20` (a collection
every 20 allocations instead of every one; DESIGN.md "Linux integration as built").

## More

- [`DESIGN.md`](DESIGN.md): architecture contract, deviations from the spec, "as built" notes
  per milestone.
- [`docs/COMPLIANCE.md`](docs/COMPLIANCE.md): the spec, statement by statement, against what is
  built (conforms / deviates / not implemented, with the test or command for each).
- [`tests/r7rs/EXCLUSIONS.md`](tests/r7rs/EXCLUSIONS.md): R7RS suite tests that do not apply
  (no exact rationals, no complex numbers, ...), each with its reason; the bignum tests run under
  `--bignum` (`tests/r7rs/bignum.scm`).
- [`bench/RESULTS.md`](bench/RESULTS.md): r7rs-benchmarks against Chez Scheme and Gambit.
