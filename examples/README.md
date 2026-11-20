# Examples

Small, complete programs. Each file starts with a comment that says how to run it and what it
shows. Run them from this directory:

```sh
cd examples
shikra run hello.scm
```

| Program | What it does | Shows |
|---|---|---|
| [`hello.scm`](hello.scm) | prints a greeting | the smallest program; `shikra size` |
| [`wordfreq.scm`](wordfreq.scm) | the most frequent words on stdin | strings, chars, string ports, a hash table on vectors, sorting, arguments |
| [`grep.scm`](grep.scm) | lines of files that contain a string | files, exit status, `guard` on file errors, a command-line tool |
| [`bank.scm`](bank.scm) | accounts and transfers that can fail | records, `raise` and `guard`, error objects, `dynamic-wind`, `parameterize` |
| [`maze.scm`](maze.scm) | the shortest path through a maze | a library of your own ([`util/queue.sld`](util/queue.sld)), mutable strings |
| [`generators.scm`](generators.scm) | generators, same-fringe, Fibonacci | re-entrant `call/cc` |
| [`streams.scm`](streams.scm) | infinite lists of primes | `(scheme lazy)`, `delay-force`, `syntax-rules` |
| [`mandelbrot.scm`](mandelbrot.scm) | the Mandelbrot set as text | flonum code without declarations |
| [`bignum.scm`](bignum.scm) | factorials and digits of pi | `--bignum` |
| [`ffi.scm`](ffi.scm) | calls the C library and [`ffi.c`](ffi.c) | `define-c`, `--link` |

Two need flags, which go before the file name (what follows it is passed to the program):

```sh
shikra run --bignum bignum.scm 1000      # 1000 digits of pi
shikra run --link ffi.c ffi.scm
```

`sh tests/run.sh examples` (from the repository root) builds every example and compares its
output with `tests/examples/NAME.out`.
