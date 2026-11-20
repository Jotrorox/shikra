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
functions there.

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
`-I dir`, `-g`, `--cc CC` (default `$SHIKRA_CC`, else `cc`), `--target TRIPLE`.
`shikra --help` lists them.

**Bignums.** By default exact integers are 63-bit fixnums and overflow raises an error (spec §3.2).
`--bignum` gives exact integers of any size (`(expt 3 10000)`, factorials, reading and printing
long numbers in radix 2, 8, 10 and 16) and the `cond-expand` feature `bignum`; results that fit are
always fixnums again, and there are still no exact rationals (`(/ 1 3)` is a flonum). Only programs
built with the flag contain bignum code; see DESIGN.md "Bignums as built".

```sh
shikra run --bignum pi.scm        # also: build, check, emit-c, repl
```

## Platforms

| Target | Executables | Notes |
|---|---|---|
| macOS arm64 | dynamically linked against libSystem (the platform requires it) | primary development platform, Apple clang |
| Linux x86-64 | static, no libc: own `_start` and system calls | GCC or clang |
| Linux arm64 | static, no libc: own `_start` and system calls | GCC or clang |

Hello world is about 3–8 KB on Linux (spec target: under 30 KB) and about 50 KB on macOS.
The `shikra` tool itself is an ordinary libc program everywhere.

**Cross-compiling.** `--target` takes the usual triple spellings (`x86_64-linux-gnu`,
`x86_64-unknown-linux-musl`, `aarch64-linux`, `arm64-apple-darwin`, ...) and sets the runtime
variant, the `cond-expand` features and the link flags. The Linux runtime needs no headers
and no libraries, so any compiler that can emit code for the target works:

```sh
shikra build --target x86_64-linux --cc "zig cc" main.scm -o app-x64
shikra build --target aarch64-linux --cc clang main.scm -o app-arm64   # clang + ld.lld in PATH
```

Without `--cc`, `cc --target=...` is tried (on macOS with `-fuse-ld=lld`).

## Tests

```sh
make test                     # everything (macOS: several minutes)
sh tests/run.sh MODE          # one part: reader unit amalg expand compiled r7rs interp
                              # stress tramp shape levels gcc (see the script's header)
sh tests/linux.sh arm64       # the suite on Linux in a podman container (native)
sh tests/linux.sh amd64 MODE  # x86-64, emulated with qemu-user (slow)
```

`tests/linux.sh` copies the working tree into a Debian container, builds the tool with the
one-liner using GCC and clang, and runs `tests/run.sh` once per program compiler.

## More

- [`DESIGN.md`](DESIGN.md): architecture contract, deviations from the spec, "as built" notes
  per milestone.
- [`tests/r7rs/EXCLUSIONS.md`](tests/r7rs/EXCLUSIONS.md): R7RS suite tests that do not apply
  (no exact rationals, no complex numbers, ...), each with its reason; the bignum tests run under
  `--bignum` (`tests/r7rs/bignum.scm`).
- [`bench/RESULTS.md`](bench/RESULTS.md): r7rs-benchmarks against Chez Scheme and Gambit.
