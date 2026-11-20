# Shikra

A small, fast ahead-of-time compiler for R7RS-small Scheme. Shikra compiles a whole program,
with every library it imports, into one C99 file and hands it to the system C compiler. The
result is a small native executable with no interpreter inside. A bytecode interpreter in the
same tool provides the REPL.

- **Small binaries:** hello world is under 10 KB as a static Linux executable without libc,
  and about 50 KB on macOS. Programs only link the runtime parts they use.
- **Fast code:** on r7rs-benchmarks it runs at parity with Chez Scheme (geometric mean 0.99×
  Chez's time over 53 benchmarks; [results](bench/RESULTS.md)).
- **No dependencies:** the compiler is about 10,000 lines of C99 and builds with one command.

## Build

```sh
cc -std=c99 -O2 src/*.c -o shikra
```

## Use

```scheme
;; hello.scm
(import (scheme base) (scheme write))
(display "hello, world")
(newline)
```

```sh
shikra build hello.scm -o hello   # compile to a native executable
shikra run hello.scm              # build and run
shikra repl                       # interactive interpreter
shikra check hello.scm            # report errors and warnings only
shikra emit-c hello.scm           # print the generated C
shikra size hello                 # break a binary down by library and procedure
```

## Platforms

| Target | Executables | C compilers tested |
|---|---|---|
| macOS arm64 | linked against libSystem | Apple clang |
| Linux x86-64, arm64 | static, no libc | GCC 14, clang 19 |

Cross-compiling works with `--target` and any C compiler that can emit code for the target.

## Limits

Shikra implements R7RS-small with a few deliberate restrictions: no exact rationals, no
complex numbers, and exact integers are 63-bit fixnums unless you build with `--bignum`.
`eval` is available in the REPL only.

## More

- [`docs/USAGE.md`](docs/USAGE.md): flags, extensions and the C interface, bignums,
  cross-compiling, tests.
- [`docs/shikra-spec-0.1.pdf`](docs/shikra-spec-0.1.pdf): the specification.
- [`docs/COMPLIANCE.md`](docs/COMPLIANCE.md): the spec, statement by statement, against what is built.
- [`DESIGN.md`](DESIGN.md): architecture and deviations from the spec.

## License

[MIT](LICENSE). The R7RS test suite in `tests/r7rs/upstream/` is from Chibi Scheme and keeps
its BSD license.
