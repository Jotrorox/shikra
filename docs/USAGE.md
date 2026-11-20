# Using Shikra

Details that the [README](../README.md) leaves out. `shikra --help` lists every flag.

## Flags

`-O0 | -O2 | -Os`, `--unsafe`, `--gc=immix|bump`, `--unicode=ascii|full`, `-I dir`, `-g`,
`--cc CC` (default `$SHIKRA_CC`, else `cc`), `--target TRIPLE`, `--link FILE`, `-l NAME`,
`--libc`, `--bignum`, `--no-cache`, `-w`.

## `shikra size`

`shikra size app` breaks an executable (Mach-O or ELF, also cross-built ones) down by library,
procedure and runtime component, so growth can be traced to its cause. The first line says how
the binary is linked (static without libc, or hosted). It needs the symbols that `shikra build`
keeps, so it refuses a stripped binary. `--all` lists every procedure, `--tsv` prints
machine-readable rows.

## Expansion cache

Expanded libraries are cached in `$SHIKRA_CACHE_DIR` (default `$XDG_CACHE_HOME/shikra`, else
`~/.cache/shikra`), keyed by their sources, their imports, the compiler build and the target.
An edited library or included file invalidates its entry. `shikra run` reuses its executable
while nothing changed, so a warm `shikra run` starts in milliseconds. `--no-cache` disables
the cache.

## Debugging with `-g`

`-g` adds the source location of the failing expression to run-time errors
(`main.scm:12:5: error: car: expected pair 5`), also inside inlined library code, and passes
`-g` to the C compiler. The generated C is kept in the cache directory for the debugger.
Without `-g` no location code or string is in the binary.

## Extensions

Five small libraries for hot paths and C. Standard programs never need them (spec §10).

| Library | Contents |
|---|---|
| `(shikra fixnum)` | SRFI 143 fixnum operations |
| `(shikra flonum)` | SRFI 144 flonum operations, with Shikra's own math functions |
| `(shikra f64vector)` | SRFI 4 `f64` vectors of raw doubles |
| `(shikra assume)` | SRFI 145 `assume`: checked, trusted with `--unsafe` |
| `(shikra ffi)` | `define-c`, a binding to a C function |

```scheme
(import (scheme base) (shikra ffi))
(define-c c-hypot "hypot" (double double) double)
```

`define-c` types: `int long double float bool char string pointer void`. Functions from your
own C files are linked with `--link file.c` (or `.o`, `.a`) and libraries with `-l NAME`. The
REPL calls C functions of its own process (the C library, libm) with up to 6 integer-class and
8 `double` arguments; everything else needs `shikra build`.

## Build modes

On Linux x86-64 and arm64 a program is *static without libc* unless it needs the C library.
It becomes a *hosted* build, linked against the C library and libm (dynamically by default,
like an ordinary C program), when it contains a `define-c` binding after tree shaking, or when
`--link`, `-l` or `--libc` is given. `shikra size` shows the mode on its first line and
`shikra emit-c` in a `build mode` comment. On macOS every build is hosted (libSystem).

## Bignums

By default exact integers are 63-bit fixnums and overflow raises an error (spec §3.2).
`--bignum` gives exact integers of any size (`(expt 3 10000)`, factorials, reading and printing
long numbers in radix 2, 8, 10 and 16) and the `cond-expand` feature `bignum`. Results that fit
are fixnums again. There are still no exact rationals: `(/ 1 3)` is a flonum. Only programs
built with the flag contain bignum code.

```sh
shikra run --bignum pi.scm        # also: build, check, emit-c, repl
```

## Cross-compiling

`--target` takes the usual triple spellings (`x86_64-linux-gnu`, `x86_64-unknown-linux-musl`,
`aarch64-linux`, `arm64-apple-darwin`, ...) and sets the runtime variant, the `cond-expand`
features and the link flags. The Linux runtime needs no headers and no libraries, so any
compiler that can emit code for the target works:

```sh
shikra build --target x86_64-linux --cc "zig cc" main.scm -o app-x64
shikra build --target aarch64-linux --cc clang main.scm -o app-arm64   # clang + ld.lld in PATH
```

Without `--cc`, `cc --target=...` is tried (on macOS with `-fuse-ld=lld`). A hosted build
(`define-c`) for another OS needs that target's C library too: `zig cc` brings one and links
against glibc dynamically; plain clang needs a sysroot.

## Platform notes

- The `shikra` tool itself is an ordinary libc program everywhere. It uses `dlsym` for
  `define-c` in the REPL; on glibc older than 2.34 add `-ldl` to the build command.
- On Linux no `-lm` is needed: the runtime brings its own math functions.
- Re-entrant continuations copy the C stack, so they need a process without hardware
  shadow-stack enforcement (Intel CET, Arm GCS).

## Tests

```sh
make test                     # everything (macOS: about 10 minutes)
sh tests/run.sh MODE          # one part: reader unit amalg expand compiled compiled2 r7rs
                              # levels interp stress tramp shape size cache debug driver
                              # examples gcc (see the script's header; `all` = everything
                              # but gcc)
sh tests/linux.sh arm64       # the suite on Linux in a podman container (native)
sh tests/linux.sh amd64 MODE  # x86-64, emulated with qemu-user (slow)
```

`tests/linux.sh` copies the working tree into a Debian container, builds the tool with GCC and
clang, and runs `tests/run.sh` once per program compiler. Under emulation the collector stress
passes run with `LINUX_ENV=STRESS_SCALE=20` (a collection every 20 allocations instead of
every one).

R7RS suite tests that do not apply (no exact rationals, no complex numbers, ...) are listed
with their reasons in [`tests/r7rs/EXCLUSIONS.md`](../tests/r7rs/EXCLUSIONS.md).
