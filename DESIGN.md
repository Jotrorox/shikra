# Shikra — architecture contract

The specification is `docs/shikra-spec-0.1.tex`. It is authoritative; read it in
full before changing anything. This file records the binding decisions the spec
leaves open, so that every part of the code base agrees. Section "As built" at
the end is maintained by whoever changes the code: keep it accurate and short.

## 1. Ground rules

- Everything is plain C99. The tool must build, warning-free, with
  `cc -std=c99 -O2 -Wall -Wextra src/*.c -o shikra` from the repo root. No
  configure step, no third-party code, no dependency beyond libc/POSIX for the
  tool itself.
- Line budgets (guidance, "about"): compiler 15,000, REPL interpreter 2,000,
  runtime 4,000. Prefer small, direct code. No speculative abstraction.
- The spec wins over convenience. If something in the spec cannot be done as
  written, do the closest faithful thing and record the deviation under
  "Deviations" below with the reason. Never silently drop a feature.
- Tests are never weakened to make them pass. A test may be excluded only when
  an *implementation restriction in the spec* (no bignums, no rationals, no
  complex, no `eval` in compiled code, compile-time rejection of provably wrong
  types, ...) makes it inapplicable, and each exclusion is listed with its
  reason in `tests/r7rs/EXCLUSIONS.md`. No special-casing of test inputs in the
  implementation.

## 2. Repository layout

```
src/        every C source of the tool; `cc src/*.c` builds it
  rt.h, rt_*.c     the runtime (shared: tool links it, programs embed it)
  prims.def        X-macro table of %primitives (shared by compiler, VM, runtime)
  embed.c          GENERATED, committed: lib/ and runtime sources as C strings
  ...              compiler, interpreter, driver
lib/        built-in Scheme libraries: lib/scheme/*.sld, lib/shikra/*.sld
tools/mkembed.c    regenerates src/embed.c (plain C99, no deps)
tests/      test suite (see §9)
bench/      benchmark harness
docs/       the spec
Makefile    `make` = regenerate embed.c + build; `make test`; `make check-embed`
```

### Embedding (see Deviations)

`shikra` is one self-contained binary: the built-in libraries (Scheme text) and
the runtime sources (C text) are inside it. C99 has no way to embed a file, so
`tools/mkembed.c` turns `lib/**` and `src/rt.h src/rt_*.c src/prims.def` into
`src/embed.c` (arrays of string literals, each literal < 4,000 chars). That file
is committed, so the documented one-line build works from a clean checkout with
no generation step. `make` regenerates it; `make check-embed` (part of
`make test`) fails if it is stale.

## 3. The runtime has two lives

The same `rt.h` / `rt_*.c` sources are used in two ways:

- **Tool mode**: compiled into `shikra` as ordinary translation units. The
  reader, the datum representation, the printer and all data primitives are used
  by the front end and by the REPL interpreter.
- **Program mode** (`SHIKRA_PROGRAM` defined): the emitter writes one C file =
  config `#define`s + `rt.h` + generated code + the needed `rt_*.c` texts, i.e.
  one translation unit, every function `static` (use a linkage macro such as
  `RT_FN`). Unreferenced runtime code then disappears at compile/link time
  (`-ffunction-sections -fdata-sections`, `-Wl,-dead_strip` / `--gc-sections`).
  Whole features are gated by config macros that the compiler emits from its
  reachability analysis (spec §9.1 "Pay only for what you use").

Platform access (write/read/open/close/exit/mmap/clock, entry point, mem*
helpers) goes through a thin layer in `rt_sys.c` only, so that the Linux
no-libc port (M6: raw syscalls, own `_start`) touches one file. Runtime code
outside `rt_sys.c` must not call libc directly — no `printf`, `malloc`,
`strtod`, `snprintf`, `setjmp.h`. Use compiler builtins behind macros where
needed (`__builtin_setjmp/longjmp`, `__builtin_add_overflow`, `musttail`), each
with a portable fallback. (The tool's compiler/driver code, which is not part
of the runtime, uses libc freely.)

### Engine boundary: data primitives are leaves

Shared runtime primitives (`car`, string ops, `write`, `equal?`, `read`, number
formatting, ports...) are **leaf C functions: they never call a Scheme
procedure**. Everything higher-order or control-related (`map`, `for-each`,
`vector-map`, `dynamic-wind`, `with-exception-handler`, `raise`, `error`,
parameters, promises, `call-with-port`, string/vector iteration with
procedures...) is written in Scheme in `lib/`, on top of %primitives. This is
what lets one library serve both engines, and it is what the optimizer shakes
and inlines.

A failing check inside a primitive calls a noreturn runtime error function that
builds the error object and hands it to the engine's raise entry:
- program mode: a C entry point that the generated code defines from the Scheme
  definition of `raise` in the library;
- tool mode (VM): a hook that unwinds to the VM dispatch loop, which then
  performs the Scheme-level `raise`.

Four control operations are engine-specific and implemented once per engine:
`%call/cc`, `%apply`, `%values`, `%call-with-values` (compiled: C stack,
`__builtin_setjmp` escapes, stack copying for tier 3; VM: explicit value stack).
The handler stack and the winders list (`dynamic-wind` + `parameterize`, one
list, spec §8.4) are runtime globals manipulated from Scheme through
%primitives; a captured continuation records both and restores/rewinds on entry.

## 4. Values (spec §6)

`obj` is a 64-bit word. Low bits: `...0` fixnum (`n << 1`), `001` pair (two
words, no header), `011` procedure (code address + captured values), `101`
other heap object (8-byte header: 8 bits type, 8 bits flags, 48 bits length),
`111` immediate (char, `#t`, `#f`, `()`, eof, unspecified, plus internal
markers such as "undefined" for letrec and "default object").

Heap object kinds: vector, bytevector, string (char count, byte count, pointer
to UTF-8 buffer; ASCII-only flag in header), symbol (pointer to name), flonum,
record (type-descriptor pointer + fields), port, promise, parameter, error
object, plus internal kinds (box, multiple-values, continuation, raw bytes
buffer, record-type descriptor, f64vector, bignum). Fixnum arithmetic never
wraps: overflow raises an error.

Procedures: `[code pointer][captured values...]`. Compiled code pointers have
the uniform signature
`obj f(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3)`, further
arguments in a runtime argument buffer. Non-tail unknown calls are plain C
calls; tail calls use `musttail` (trampoline fallback when unavailable).
Raw non-`obj` words stored in precisely-scanned objects must look like fixnums
(low bit 0) or live in raw (non-scanned) object kinds.

## 5. Memory (spec §7)

- Allocation API is one small interface in `rt.h` (pair, headered object, raw
  buffer) with an inlined bump fast path; `rt_gc.c` holds both collectors:
  `immix` (default) and `bump` (`--gc=bump`, never frees).
- Immix: 32 KB blocks, lines, side mark bits and an object-start bitmap (pairs
  have no header). Large objects are allocated separately.
- Roots: the C stack and registers, scanned conservatively — a stack word keeps
  an object alive if it points at *or into* it, tagged or untagged, because the
  C optimizer may keep only a derived pointer; plus registered root ranges (the
  program's global variables, runtime globals). Heap objects are scanned
  precisely. Static data (literals, static symbols, closure-free procedures) is
  never scanned or freed; any pointer outside the heap is ignored by the marker.
- Stress mode: collect at every allocation (env `SHIKRA_GC_STRESS=1` in a
  build made with `--gc-stress`, a hidden testing flag).
- In the tool: compile commands run with collection disabled (batch program,
  frees nothing). In the REPL, front-end allocations (datums, expander state,
  constants) are made in *immortal* mode: never reclaimed, and scanned as roots.

## 6. Front end (passes 1–3, shared by compiler and REPL)

- **Read**: the runtime reader (`rt_read.c`) produces ordinary runtime datums.
  In tool mode it records `file:line:column` for every pair in a side table.
- **Resolve libraries**: `define-library`, `export` (with rename), `import`
  (`only`/`except`/`prefix`/`rename`), `begin`, `include`, `include-ci`,
  `include-library-declarations`, `cond-expand`. `(import (a b))` → `a/b.sld`
  searched in the program's directory, each `-I` dir, then the embedded
  built-ins. Features: `r7rs shikra ieee-float`, OS, architecture, and
  `full-unicode` with `--unicode=full`; never `ratios` or `exact-complex`.
- **Expand**: hygienic `syntax-rules` (nested ellipsis, custom ellipsis,
  `(... ...)`, `_`, literals compared by binding, vector and improper patterns,
  tail patterns, `let-syntax`, `letrec-syntax`, internal `define-syntax`,
  `syntax-error`). Output is **core forms as datums**:

  ```
  top   ::= (define <global> core) | core
  core  ::= <variable> | (quote datum) | (lambda formals core) | (if c t e)
          | (set! var core) | (letrec* ((var core) ...) core) | (begin core ...)
          | (core core ...)            ; call
          | (%prim core ...)           ; primitive application, exact arity
  ```

  Local binders are renamed apart by the expander (hygiene); globals are unique
  symbols that carry their library. `%prim` names come from `src/prims.def`;
  primitives are not first-class — the library wraps them
  (`(define (car x) (%car x))`) and the compiler inlines the wrappers.
  `case-lambda`, records, `call/cc`, `values`, `apply` lower to dedicated
  %primitive forms. `,expand` prints core forms with renaming stripped.

The built-in libraries are ordinary `.sld` files under `lib/`, embedded as
text; they import the hidden library of %primitives.

## 7. Middle and back end (passes 4–8)

- **IR**: A-normal form with join points (labelled blocks with parameters); one
  tagged union with a `kind` field; every pass is a `switch` over the kind.
  Designed at M0 so that M4 (analysis/optimizer) adds passes instead of
  rewriting. Compiler memory: arenas, nothing freed.
- **Normalize**: unique variables, `letrec*` grouping (simple / procedure /
  complex), boxes for variables that are both captured and assigned, → IR.
- **Analyze / Optimize** (M4): one monovariant whole-program flow analysis
  (procedures per call site, types per variable; lattice in spec §5.2), then
  the optimizations of spec §5 pass 6.
- **Emit**: one C file (see §3), then `cc`. Until M4 the backend is naive:
  every procedure has the uniform signature and every primitive is checked.
- Closed world: a top-level definition never `set!` is a constant; `set!` on an
  imported binding is a compile error.

## 8. Interpreter (M3)

Front end → core forms → compact bytecode → VM with its own value stack (no C
recursion for Scheme calls; tail calls in constant space; continuations copy
the VM stack). Open top level (globals in a table, redefinable). Always
checked. Uses the same runtime and the same Scheme library sources as compiled
programs. Never linked into compiled programs.

## 9. Tests

```
tests/run.sh [compiled|interp|stress|all]   driver used by `make test`
tests/cases/NAME.scm + NAME.out             program + expected stdout (+ NAME.exit)
tests/errors/NAME.scm + NAME.err            expected compile diagnostics (substring)
tests/r7rs/                                 adapted Chibi r7rs-tests, split by library
```

Every feature lands with a test. From M3 on, every case runs under both
engines and the outputs must be identical.

## 10. Deviations from the spec

- *"No generated sources"*: `src/embed.c` is generated but committed, so the
  build itself has no generation step (§2). C99 cannot embed text otherwise.
- *Conservative scan accepts interior and untagged pointers*, not only tagged
  pointers to object starts (§5): required for correctness under `cc -O2`.

## 11. As built

### M0 part A (build, runtime foundation, reader)

**File map**

```
Makefile            make | make test | make check-embed | make clean
tools/mkembed.c     FILE... -> src/embed.c on stdout (sorted, chunks < 4000 bytes)
src/rt.h            values, object layouts, linkage macros, the whole runtime API,
                    inline fast paths (alloc, cons, car, vector-ref, fixnum + - * < >)
src/rt_sys.c        platform layer (only libc user); program-mode main(); M6 stub
src/rt_gc.c         bump allocator (two streams), roots registry, immortal flag
src/rt_core.c       rt_init/rt_exit, error path, lists, eqv/equal, symbols+intern
                    table, vectors, bytevectors, boxes, records, error objects, values
src/rt_str.c        UTF-8 strings, chars
src/rt_num.c        fixnum/flonum arithmetic, rounding, number<->string
src/rt_io.c         ports (fd + string), printer (write/display)
src/rt_read.c       reader, datum labels, source-location table (tool mode)
src/prims.def       X-macro table of every %primitive (format documented in the file)
src/embed.h/.c      embedded files (embed.c GENERATED, committed)
src/util.h/.c       tool code: die, arena, sbuf, htab, read_file, embed_text
src/main.c          driver: commands + flags parsed into `options`; all commands are
                    stubs except hidden `shikra debug-read FILE`
tests/run.sh        modes reader | unit | amalg | compiled | interp | stress | all
tests/reader/       NAME.scm + NAME.out (printed datums; must also read back to
                    itself) or NAME.err (expected stderr substring)
tests/unit/         rt_unit.c (C-level runtime tests), amalg_prog.c.in (program-mode
                    self-check: SHIKRA_PROGRAM + rt.h + it + rt_*.c, -Werror)
```

Runtime is ~2,250 lines (rt.h 490), leaving ~1,750 of the 4,000 budget for Immix
(~1,200) and the M1/M5 additions (binary/file ports, write-shared, Unicode tables).

**Linkage.** Every runtime function is declared `RT_FN`, globals `RT_VAR` (definition)
/ `RT_EXTVAR` (declaration in rt.h), inline helpers `RT_INLINE`. In program mode they
are all `static` + unused-attribute. Each rt_*.c starts with
`#ifndef SHIKRA_RT_H / #include "rt.h"`. File-local helpers carry a per-file prefix
(`core_`, `str_`, `num_`, `dd_`, `io_`, `rd_`) because the program is one TU. rt.h
defines `_POSIX_C_SOURCE`/`_DARWIN_C_SOURCE` and includes only stddef/stdint, so it
must come first in the amalgamation (and first in tool .c files that include libc).

**Program-mode contract** (what the emitter must provide/assume): the generated code
defines `static void rt_program_main(void)` (rt_sys.c's `main` calls `rt_init`, then
it, then `rt_exit(0)`) and `static void rt_engine_raise(obj e)` (noreturn; the Scheme
`raise`; may fall back to `rt_uncaught(e)`). Static symbols must be entered with
`rt_intern_static(sym)` at startup so `string->symbol` finds them. Static literals use
the RT_HDR macro and the structs in rt.h; set `RT_F_RO` on literal strings, vectors
and bytevectors (mutators check it).

**Values.** `obj` is `struct rt_obj_ *` (not an integer) so that static initializers
like `(obj)((char *)&lit + 5)` are C99 address constants. Immediates: `#f`=0x007,
`#t`=0x107, `()`=0x207, eof 0x307, unspecified 0x407, undefined 0x507, default
0x607; chars `(cp << 8) | 0x0F`. Heap types and their scan classes are listed in
rt.h (1..15 all-obj, 16..31 raw, 32+ special). Decisions not in §4:
- Procedures: the obj points at the code word; the header (type RT_T_PROC, length =
  number of captured values) is the word *before* it (`RT_PROC(x)`,
  `RT_PROC_OBJ(p)`, `rt_make_proc`). A call is one load: `RT_PROC(f)->code(f, ...)`.
- Strings: `{hdr, intptr_t chars, intptr_t bytes, char *buf}`; buf NUL-terminated,
  may point to static data, is replaced by `string-set!` when the UTF-8 width
  changes. `RT_F_ASCII` is maintained as `chars == bytes`.
- Records: `[hdr][rtd][fields...]`, length = 1 + nfields; rtd = `[name][field-name
  vector]`. Error objects: `[message][irritants][loc]`, kind in header flags
  (RT_ERR_GENERAL/FILE/READ); `loc` is #f or a "file:line:col" string.
- Ports: fixed struct `rt_port` (fd or string buffer, pos/len/cap, line/col, name).

**Allocation API.** `rt_alloc(hdr, bytes)` (writes header, returns untagged pointer;
tag with `RT_TAGPTR`), `rt_cons(a, d)` (pair stream), `rt_alloc_raw(n)` (unscanned
buffer). Pairs and headered objects use separate bump streams (`rt_pp/rt_plim`,
`rt_hp/rt_hlim`) so Immix can keep pairs in pair-only blocks where every 16-byte
cell is an object start; `RT_GC_NOTE_START(p)` is the hook for an object-start
bitmap. Slow path `rt_alloc_slow` takes 1 MB chunks from `rt_sys_mem` (large objects
get their own mapping). `rt_gc_add_roots(start, end)`, `rt_gc_set_immortal`,
`rt_gc_collect` exist; under bump they only record. Rule: initialise every field of a
new object before the next allocation. The stack base is passed to `rt_init`.

**Error path.** `rt_error(who, msg, irritants)` builds message "who: msg";
`rt_type_error(who, "pair", x)` → "car: expected pair" with irritant x;
`rt_range_error`. All end in `rt_raise(e)`: program mode → `rt_engine_raise`; tool
mode → `rt_raise_hook` if set (must not return: longjmp), else `rt_uncaught(e)`, which
flushes stdout and prints `Error: msg irritant...` (`loc: error: msg ...` if the error
has a location) and exits with status **70**.

**Primitives.** Every primitive is a leaf `obj f(obj, ...)` named in prims.def with
type letters and PF_PURE/ALLOC/FAIL/EFFECT flags. Arity is exact; optional
arguments (start/end, ports) are supplied by lib/. Binary arithmetic/comparison
only; the library folds variadic calls. `%string-compare` returns -1/0/1 and the
library builds `string=?`/`string<?`... on it. Record ref/set take the rtd and a
constant index. Not in the table (engine-specific): `%call/cc %apply %values
%call-with-values`.

**Reader + locations.** `rt_read(port)` returns a datum or `RT_EOF`; errors are
error objects of kind READ with `loc` = "file:line:col" when the port has a name
(file ports), so an uncaught reader error prints `file:line:col: error: msg`. In tool
mode `rt_srcloc_enable(1)` makes the reader record (file, line, col) for every pair
it creates (list pairs: the first at its `(`, later ones at their element; quote
abbreviations at the quote char); `rt_srcloc_lookup(pair, &file, &line, &col)`.
The table lives in OS memory and is not scanned: datums must be immortal (REPL) or
collection disabled (compiler). `1/2` reads as 0.5, `6/3` as exact 2; an exact
literal outside the fixnum range or `#e1.5` is a read error ("cannot be represented").

**Ports.** stdin/stdout/stderr (`rt_stdin` ...), 4 KB buffers; stderr flushes after
each write and flushes stdout first; reading stdin flushes stdout; `rt_exit` flushes
both. String ports in both directions; `rt_open_input_file/output_file` exist (raise
FILE errors). Textual only so far; `RT_PF_BINARY` is reserved.

**Open issues / known gaps**
- Float printing/parsing is interim (`TODO(M5)` in rt_num.c): double-double scaling.
  300k random doubles round-trip exactly and print shortest for all normal numbers;
  subnormals may print one digit longer; parsing truncates beyond 19 significant
  digits, so it is not guaranteed correctly rounded.
- `equal?` and `write` do not terminate on circular data (M5: union-find equal?,
  write-shared/datum labels in rt_print).
- Only the standard ports are flushed at exit; unclosed file output ports are not.
- `exact`/integer division on flonums outside ±2^62 raise an error.
- The collector is bump-only; `--gc` is parsed but ignored until M2. The tool has
  no Immix either, so `rt_gc_set_immortal` is a no-op.
- M6 (no-libc Linux) must also provide memcpy/memset symbols: the C compiler may emit
  calls to them for struct copies even though the runtime calls only rt_memcpy.
