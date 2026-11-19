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
`guard` and `parameterize` are `syntax-rules` macros in `lib/` over library
procedures, so both engines share them too (§11 M1 part A).

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
tests/run.sh [compiled|r7rs|interp|stress|expand|all]   driver used by `make test`
tests/cases/NAME.scm + NAME.out             program + expected stdout (+ NAME.exit,
                                            + NAME.err: expected stderr substring,
                                            + NAME.in: stdin, else /dev/null)
tests/errors/NAME.scm + NAME.err            expected compile diagnostics (substring)
tests/expand/NAME.scm + NAME.out            core forms as `,expand` prints them
tests/r7rs/                                 adapted Chibi r7rs-tests, split by library
tests/r7rs/expected/FILE.ENGINE             pinned summary line + failing tests (r7rs mode)
```

Every feature lands with a test. From M3 on, every case runs under both
engines and the outputs must be identical.

## 10. Deviations from the spec

- *"No generated sources"*: `src/embed.c` is generated but committed, so the
  build itself has no generation step (§2). C99 cannot embed text otherwise.
- *Conservative scan accepts interior and untagged pointers*, not only tagged
  pointers to object starts (§5): required for correctness under `cc -O2`.
- *Known top-level procedures exist from program start* (M0 part B): they are static
  closures, so calling one before its `define` has been reached works instead of
  raising "used before its definition" (spec §3.3 asks safe mode to detect "it is an
  error" situations). Other globals and early `letrec*` references are checked. M4 can
  detect the remaining case statically (a call reachable from an earlier top-level form).
- *Quoted pairs are writable* (§3.2 "literal constants are read-only"): pairs have no
  header for `RT_F_RO`, so `set-car!` on a quoted list is not detected. Literal strings,
  vectors and bytevectors are read-only. M2 may place literal pairs in a const section
  (mutation then faults instead of raising) or check an address range.
- *Tree shaking spares user code* (§9 "anything unreachable ... is never emitted"):
  every form of the program and of user libraries is compiled so that the middle end's
  diagnostics (arity of known calls) cover all user code; only built-in library code is
  shaken. The C compiler and linker still drop unreferenced static functions. M4's
  shaker may drop unused user definitions after diagnostics.
- *No trampoline fallback yet* (§8.1): without `musttail` (`RT_MUSTTAIL` empty) tail
  calls rely on the C compiler's sibling-call optimisation, which `-O2` performs for these
  same-signature calls but `-O0` does not. TODO(M4).
- *The receiver of a tier-2 `call/cc` is not called in tail position* (R7RS 3.5 requires
  it; M1 part A): `rt_call_cc`'s C frame holds the jump target while the receiver runs, so a
  loop that recurses through `call/cc` grows the C stack. Tier-1 sites in tail position are
  proper (the receiver body is inlined). Fixing it needs heap continuations (M5 tier 3).
- *`guard` clause tests see the raiser's parameter values* (R7RS 4.2.7 says the guard's
  dynamic environment; M1 part A): spec §8.3 evaluates the clauses on top of the raising
  frame, before unwinding; they run with the guard's handler stack, but the winders list
  (hence parameterize bindings) is still the raiser's. Bodies of matching clauses run after
  unwinding, in the guard's environment. Setting the winders list to the guard's for the
  tests would skip the raiser's after thunks if a test escaped.
- *A literal `call-with-values` receiver given a provably wrong number of values is a
  compile error* (M1 part A), like a known call with the wrong argument count (§11.3):
  `(call-with-values (lambda () (values 1 2 3)) (lambda (a b) a))`.
- *No exact rationals beyond `/`* (M1 part B): `expt` with a negative exact exponent follows
  `/` (flonum unless it divides); `string->number`/the reader give `#f`/a read error for ratio
  parts outside the fixnum range. See "M1 part B, Decisions" for the remaining small choices.
- *Hello world on macOS arm64 is 69 KB*, not ~50 KB (§9.4, M1 part A): `raise` is always
  linked (runtime errors are raised through it) and `display` reads the
  `current-output-port` parameter; the text grew by ~2.5 KB but crossed a 16 KB page. M4's
  type-level shaking and inlining are expected to win this back.

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
FILE errors). Binary and bytevector ports: M1 part B.

**Open issues / known gaps**
- Float printing/parsing is interim (`TODO(M5)` in rt_num.c): double-double scaling.
  300k random doubles round-trip exactly and print shortest for all normal numbers;
  subnormals may print one digit longer; parsing truncates beyond 19 significant
  digits, so it is not guaranteed correctly rounded.
- `equal?` and `write` do not terminate on circular data (M5: union-find equal?,
  write-shared/datum labels in rt_print).
- (Fixed in M1 part B: every open output port is flushed at exit.)
- `exact`/integer division on flonums outside ±2^62 raise an error.
- The collector is bump-only; `--gc` is parsed but ignored until M2. The tool has
  no Immix either, so `rt_gc_set_immortal` is a no-op.
- M6 (no-libc Linux) must also provide memcpy/memset symbols: the C compiler may emit
  calls to them for struct copies even though the runtime calls only rt_memcpy.

### M0 part B (front end, IR, naive C backend, driver)

**File map** (compiler ≈ 3,700 lines of C; libraries ≈ 650 lines of Scheme)

```
src/front.h       front-end API and data: pmap, prim_table (prims.def + control forms),
                  keywords, binding/frame/global/library, entry points, diagnostics
src/library.c     pass 2: define-library, import sets, export, include(-ci),
                  include-library-declarations, cond-expand + features, library search
                  and graph (cycle detection, init order), (shikra primitives), program
                  loading, REPL entry points, cerror/locations, read_source, pmap
src/expand.c      pass 3: aliases (hygiene), frames, syntax-rules, derived forms, body
                  and top-level scanning, unparse
src/ir.h          the IR (one tagged union) and the middle/back-end API
src/normalize.c   passes 4 and 6-lite: tree shaking, variable analysis, letrec*
                  grouping, boxes, known calls, ANF + join points, closure layout,
                  C names, def/use counts, IR printer
src/emit.c        pass 7: the C translation unit
src/main.c        driver: build, run, check, emit-c, debug-read/-expand/-core/-ir
lib/scheme/base.sld          (scheme base) = include-library-declarations of base/*.scm
lib/scheme/base/*.scm        one topic per file: syntax errors equivalence numbers lists
                             symbols chars strings vectors bytevectors control io
lib/scheme/{write,cxr,case-lambda,process-context}.sld
```

**Front end.** Identifiers are symbols. Interned symbols come from source; *aliases*
are uninterned symbols made by macro expansion (and by the C rewrites), each mapped in a
side table to `{original identifier, environment of the macro}`. `lookup(env, id)`
searches the frame chain for `id` itself, and if `id` is an alias and unbound there,
continues with the original identifier in the alias's environment (explicit renaming).
Frames map identifier objects to `binding`s; imports share the exporter's `binding`
objects, so "same binding" is pointer equality across libraries (literal matching,
`free-identifier=?`). Kinds: `B_CORE` (keyword or auxiliary syntax, `K_*`), `B_MACRO`,
`B_GLOBAL`, `B_LOCAL`, `B_PRIM` (`PI_*`). The hidden library `(shikra primitives)` is
built in C (library.c) and exports every keyword, `else => ... _ unquote
unquote-splicing`, every prims.def name and the control primitives `%apply %values
%call-with-values %call/cc`. Built-in libraries import it.

Library resolution: `(a b)` → `a/b.sld` in the program's directory, each `-I`, then the
embedded `lib/a/b.sld` (file name `<builtin>/a/b.sld` in diagnostics; includes inside it
resolve to embedded files). Libraries are loaded depth-first on first import; a library
seen again while loading is a cycle (error); completion order = initialisation order
(`front_order`, program last). Declarations are processed in order (imports bind
immediately), then the collected body is expanded, then exports are resolved. A program
is its leading `import` forms (a leading `cond-expand` is spliced so it may contain
imports), then the body. Features (spec §3.4): `r7rs shikra ieee-float`, OS
(`darwin macos` / `linux gnu-linux`, plus `unix posix`), architecture (`x86-64` or
`arm64 aarch64`), `little-endian lp64`, and `full-unicode` with `--unicode=full`; with
`--target` they are derived from the triple. `cond-expand` works as a library
declaration, a body/top-level form (splices) and an expression.

Expansion: bodies and top levels are expanded in two phases: a scan that expands macro
uses in head position, splices `begin`, binds definitions (`define`, `define-syntax`,
`define-record-type` and `define-values` rewrite to defines, `include`, `cond-expand`)
in the frame as they are found; then the right-hand sides and expressions are expanded.
So forward references work at every level. A body becomes `(letrec* ((v e) ...) expr)`;
an expression before the last definition becomes a binding of a dummy variable `_.N`
(evaluation order is kept); a body must end with an expression. Top-level definitions
of a library or program are globals; redefinition is an error unless the frame is open
(REPL); a top-level definition may shadow an imported name. A definition whose name was
introduced by a macro (an alias) is hygienic: only identifiers from the same expansion
see it.

Derived syntax is lowered in C (expand.c, "derived syntax"): each `rw_*` builds a source
form over `kw()` aliases into `(shikra primitives)` (hygienic, uncapturable) and expands
it. Native: `let` (named: `((letrec* ((n (lambda ...))) n) init ...)`), `let*`, `letrec`
(= `letrec*`), `cond` (`=>`), `case` (`=>`, `%eqv?` chains), `and`, `or`, `when`
(`(if t (begin ...))`), `unless`, `do`, `quasiquote` (levels, vectors, splicing via
`%append2`; constant parts fold into one quote), `define-record-type` (`%make-rtd`,
`%make-record`, `%record-ref/-set!` with constant indices), `case-lambda` (core
`%case-lambda`), `let-values`, `let*-values`, `define-values` (via
`%call-with-values`), `let-syntax`, `letrec-syntax`, `syntax-error`. Not yet: `delay`,
`delay-force` (M5): clean "not implemented yet (milestone M5)" errors. (`guard` and
`parameterize` are `syntax-rules` macros of `(scheme base)` since M1 part A.) Rationale for C over syntax-rules: located, specific diagnostics
("malformed let binding (y)") and no dependency of basic syntax on the macro engine.

syntax-rules: patterns are matched directly (no compilation); bindings are a list of
`(var, depth, value)` with depth-n values as nested lists. Supported: nested ellipsis,
`x ... ...` in templates, custom ellipsis, `(... ...)`, `_`, literals (by binding; a
literal named like the ellipsis wins), vector and improper patterns, tail patterns
after an ellipsis. Template symbols are renamed once per expansion (same symbol, same
alias).

**Core forms** (expander output; `debug-core` prints them raw):

```
top  ::= (define <global> core) | core
core ::= <var> | (quote datum) | (lambda formals core) | (%case-lambda (formals core) ...)
       | (if c t) | (if c t e) | (set! <var> core) | (letrec* ((<local> core) ...) core)
       | (begin core ...) | (core core ...) | (%prim core ...)
```

Keywords and `%prim` heads are interned symbols; every variable is an uninterned symbol:
locals `name.N` (N unique per compilation), globals `lib/id:name` (e.g.
`scheme/base:car`, `main:fib`). `front_global(sym)` / `front_local_name(sym)` map back.
Quoted data has aliases stripped. Constants are always quoted. Every pair the expander
creates for a core form gets the source location of the form it came from (or of the
innermost located form: the macro use) via `rt_srcloc_set`, so the middle end can
report `file:line:col`. `unparse(x, port, raw=0)` strips renaming and prints `(quote x)`
as `'x` (self-evaluating data bare): `(when (> 1 0) 'yes)` → `(if (> 1 0) (begin 'yes))`.

**REPL entry points** (front.h): `front_repl_new` (an open top level: unbound variables
become globals, redefinition allowed), `front_repl_import`, `front_repl_expand` (one
form → list of top-level core forms). Checked with a throwaway harness only; M3 wires
them up.

**The IR** (ir.h; read its header comment first). ANF with join points. Terms: `LET v =
rhs; body` (v NULL: effect), `FIX` (mutually recursive closures), `LETJ` (declare join
point, then body), ending in a tail: `RET atom`, `CALL ... tail=1`, `JUMP j(atoms)`,
`IF atom term term`. Rhs: atoms (`CONST`, `REF`), `PRIM` (op = prim_table index),
`CALL` (non-tail; `ctl` = `CTL_APPLY`/`CTL_CWV` for `%apply`/`%call-with-values`),
`LAMBDA` (closure creation), `SET`. A value-position `if` becomes a join point with one
parameter whose body is the continuation. Every variable is bound once, has a unique id,
an `owner` procedure, flags (`V_GLOBAL V_ASSIGNED V_CAPTURED V_BOXED V_CHECK`),
def/use counts (`ir_count`), `known` (bound once to a procedure, never assigned), a
`type` slot (`T_*` lattice bits, 0 = unknown) for M4. Calls carry `known` = resolved
callee (for case-lambda: the clause). Procedures carry arity, params, `entry` (join
point for self tail calls, parameters = the procedure's parameters), free-variable
layout, `parent`, case-lambda `clauses`/`dispatch`, `self` variable, `is_static`, `lib`.
`prog->vars` lists every variable by id; `prog->lambdas` every procedure in creation
order (parents before children). For M4: def/use counts and `known` are the hooks for
inlining and contification (e.g. a named-let loop whose only uses are tail calls can
become a join point of its caller), `type` for the flow analysis, `prim_table` flags
(`PF_PURE/ALLOC/FAIL/EFFECT`) and type letters for folding and check elimination.

**Normalization** (normalize.c, `ir_build`):
1. *Tree shaking* on core forms: roots are every form of the program and of user
   libraries, and every built-in library form whose rhs is not pure (pure = quote,
   variable, lambda, or `PF_PURE` primitives of pure operands); a definition is kept iff
   its global is reachable from a root (globals referenced in kept forms). Hello world
   carries only `display`, `newline` and the program body.
2. *Variable analysis*: one `ir_var` per binder; assigned (any `set!`) and captured
   (referenced from another lambda; direct `((lambda ...) args)` counts as a let).
   Assigned + captured ⇒ `V_BOXED`: the variable holds a box (`%make-box`, reads
   `%box-ref`, writes `%box-set!`); a boxed parameter gets a fresh raw parameter and the
   body starts by boxing it. Assigned but not captured stays a C local.
3. *Known top-level procedures*: a global defined once, never `set!`, by a `lambda` or
   `%case-lambda`. Shells (arity, parameters) are made before any body is normalized.
4. *letrec\**: bindings are *simple* (quote, or a variable outside the group: hoisted),
   *lambda* (consecutive runs become one `FIX`) or *complex* (evaluated in order). A
   binding referenced by an rhs before its own position (for lambdas: before its run
   starts) is *early*: bound to `RT_UNDEF` first (boxed if captured), assigned at its
   position, and read with an undefined check (`V_CHECK`) until then; an early lambda is
   demoted to complex (fixpoint). Globals: a read is checked unless the global is a
   known procedure, or the read is in non-static code of a top-level form after the
   defining form.
5. *Calls*: `((lambda formals body) args)` binds like `let` (arity checked; a rest
   parameter gets a consed list). A call whose callee variable is known resolves the
   procedure/clause at compile time; a wrong argument count is a compile error at the
   call (`f called with 2 arguments, but it takes 1`). A known tail call to the procedure
   being normalized (no rest parameter) becomes `JUMP entry`.
6. *Closure layout*: free variables per procedure (a case-lambda's clauses share the
   union); a reference to the procedure's own `self` variable is not captured (it is the
   `self` argument). No free variables ⇒ static closure.

**Calling convention** (rt.h "compiled calling convention"). Uniform signature
`obj f(obj self, intptr_t argc, obj a0..a3)`; arguments beyond a3 go in `rt_args[]`
(`RT_ARGBUF` = 64 static slots registered as GC roots; `rt_args_reserve` grows it,
TODO(M2): root the grown buffer), stored right before the call, copied out in the
callee's prologue. Rest lists: `rt_rest_list(argc, nreq, a0..a3)`. Every non-clause
procedure checks `argc` (`rt_arity_error(name, argc)` → "fib: wrong number of
arguments 3"). Unknown calls check `RT_CALLABLE(f)` ("not a procedure 5") and call
`RT_PROC(f)->code(f, n, ...)`; known calls call the C function directly with the closure
as `self`. A case-lambda is a dispatcher function (tests argc in clause order,
`RT_MUSTTAIL`-calls the clause with the same `self`); clauses do not re-check.
`%apply` / `%call-with-values` spread a list / a values object with `rt_spread` /
`rt_spread_values` into `r_[4]` + `rt_args`. `values` of one value is the value;
otherwise `%values` / `%make-values` build a values object (TODO(M1/M4): registers for
known receivers, spec §8.5).

**Tail calls.** Self tail calls of known procedures are `goto entry` after a parallel
assignment of the parameters. Every other tail call is `RT_MUSTTAIL return f(...)`
(`__attribute__((musttail))`, clang and GCC 15); verified in constant stack for 10^7
iterations at `-O0` and `-O2` (tests/cases/tail_calls.scm). Fallback: see Deviations.

**Emitted C** (emit.c): `#define SHIKRA_PROGRAM 1` + config (`RT_UNSAFE RT_DEBUG
RT_GC_BUMP RT_UNICODE_FULL RT_GC_STRESS`, from the flags; the runtime does not read them
yet) + rt.h + program + `rt_sys.c rt_gc.c rt_core.c rt_str.c rt_num.c rt_io.c rt_read.c`.
Program: literals as static data (`L<n>`: `rt_string`/`rt_pair`/`rt_flonum` (hex float)/
vector/bytevector structs, `RT_F_RO` on strings, vectors, bytevectors; symbols are
static `rt_symbol`s listed in `rt_static_syms` and entered with `rt_intern_static` at
startup), `rt_g[]` (non-procedure globals, initialised to `RT_UNDEF`, one root range),
prototypes, static closures `C_...` (`RT_SPROC(c)` is the address-constant obj), one
function per procedure, `rt_program_main` (calls `P_init`, the program body) and
`rt_engine_raise` (`rt_uncaught` until M1). Names: procedures
`P_<library>__<name>[__<inner>...]` (`P_scheme_base__list_tail`, `P_main__fib`,
`P_main__sum_to__loop`; operator names spelled out: `P_scheme_base__plus`),
case-lambda clauses `<dispatcher>__<k>`, the body `P_init`, locals `v_<name>_<id>`.
Variables never read are not declared and their assignments are dropped (jump arguments
count as uses only if the join parameter is used), so the C is warning-free under
`-Wall -Wextra` (checked for every test case).

**Driver.** `build` writes the C to `<cache>/build-<pid>.c` and runs
`cc [--target=T] -std=c99 -O{0,2,s} -ffunction-sections -fdata-sections [-g]
-Wl,-dead_strip|--gc-sections -o OUT FILE.c` (`--cc` may contain arguments; the C file is
kept if cc fails). `run` builds at `-O0` into `<cache>/run-<hash>` (hash = FNV-1a of the
C text, compiler and flags; built under a pid-unique name and renamed, so concurrent runs
do not collide and repeated runs skip cc), then `execv`s it with the remaining
arguments. Cache = `$XDG_CACHE_HOME/shikra` or `~/.cache/shikra`. `check` = front end +
`ir_build` (all diagnostics, no C). Compile errors exit 1; runtime errors in the
compiler (reader errors, malformed data reaching a primitive) become located compile
errors (`compile_errors_init`).

**How to add ...**
- *a primitive*: a leaf C function in `rt_*.c` (declared `RT_FN` in rt.h), a row in
  `src/prims.def` (exact arity, type letters, flags), `make`. The expander, IR and
  emitter pick it up from the table; use it from `lib/` as `(%name args)`.
- *a library procedure*: a definition in the topic file under `lib/scheme/base/` (or a
  new `.sld`) plus its name in that file's `export`; optional arguments via
  `case-lambda`; `make` (regenerates embed.c), add a test, commit embed.c.
- *a derived form*: a keyword in `front.h` (`K_*`) and `keyword_names` (library.c), an
  `rw_*` rewrite in expand.c dispatched from `expand_core` (or a definition-producing
  case in `scan_form`), export it from the relevant `.sld`, add tests in `tests/cases`
  and a `tests/errors` case for its malformed use.
- *a control primitive* (engine-specific): a `PI_*` after `NPRIMS` with its
  `prim_table` row; normalize.c turns it into an `IR_CALL` with a new `CTL_*`; emit.c
  emits it (see `CTL_CALLCC`).

**Tests.** `make test` (= `check-embed` + `tests/run.sh all`): reader, unit, amalg,
expand (1), compiled (16 programs, each also compiled with `-Wall -Wextra -Werror`),
errors (24 diagnostics). `tests/run.sh compiled` alone takes ~5 s.
Debugging: `shikra debug-expand|debug-core|debug-ir|emit-c FILE`.

**R7RS suite status** (at the end of M0; see M1 part B for the current state).
`shikra check tests/r7rs/base.scm` stopped at the shim's first `guard` (`tests/r7rs/chibi/test.sld:153:7: error: guard is not implemented yet
(milestone M1)`). In a scratch copy with the M1 features stubbed (`guard`,
`parameterize`, `call/cc`, `dynamic-wind`, exception handlers, `expt`,
`numerator`/`denominator`, binary ports, `features`, `test-error`), the whole file
compiles and runs into section 6.10; every failure before that is a test of a stubbed
procedure.

**Known gaps / next steps**
- (M1 items done in parts A and B.) `command-line` and the other optional libraries
  (`(scheme char)` ...) are M5.
- M4 (performance): every variable is a C local of type `obj`, every primitive is the
  checked runtime call, named-let loops are separate C functions entered by a direct
  call (not contified), closures for local procedures with free variables are
  allocated, `values` to an unknown receiver boxes (literal receivers: M1 part A).
- `strip_syntax` and the literal emitter do not handle circular quoted data (datum
  labels in literals); `equal?`-cycles are a runtime M5 item too.
- Macro expansion recurses on the C stack (very deep recursive macros could overflow);
  a non-terminating macro is cut off after 100,000 steps per form.
- Diagnostics are reported at the first error (no recovery); no warnings yet (unused
  imports/definitions, spec §11.3).
- Commits of this milestone are unsigned: the 1Password SSH signing agent was not
  available while the owner was away.

### M1 part A (control: dynamic state, exceptions, continuations, multiple values)

**File map** (runtime +120 lines → ~2,470; compiler +330 → ~4,190; Scheme library +200)

```
src/rt_core.c   rt_winders / rt_handlers (GC roots) and their leaf %primitives,
                %parameter-binding, %continuation-live?, %values-check/-ref/-tail,
                %uncaught; rt_call_cc + core_cont_code (program mode only): %call/cc
src/rt_sys.c    rt_sys_catch / rt_sys_throw: the only setjmp user
src/normalize.c "control forms the normalizer open-codes": rewrite_aliases, cwv_inline,
                tier1_site/k_ok; norm_callcc1/kjump (tier 1), norm_cwv/norm_mv/mv_jump/
                mv_spread (receiver join points); raise kept as a root; dead procedures
                dropped; ir_count counts copies into unused variables lazily
src/emit.c      CTL_CALLCC; rt_engine_raise -> (scheme base) raise; (%values x) is x
lib/shikra/dynamic.sld          internal library: travel-to!, the winders walk
lib/scheme/base/errors.scm      with-exception-handler raise raise-continuable error guard
lib/scheme/base/control.scm     dynamic-wind call/cc make-parameter parameterize (+ M0)
lib/scheme/base/io.scm          current-input/output/error-port are parameters
lib/scheme/process-context.sld  exit unwinds first; emergency-exit does not
```

**Dynamic state** (spec §8.4). `rt_winders` is one Scheme list, innermost first, of
`(before . after)` frames (dynamic-wind; car is a procedure) and `(cell . value)` frames
(parameterize; car is a pair). `travel-to! to` (Scheme) unwinds from the current list to
the common tail, setting the list to the outside of each frame before calling its after
thunk, then rewinds outermost first (before thunk called outside its frame, frame entered
after it returns) -- R7RS 6.10, and the after thunks see the right parameter values.
`dynamic-wind`: call before, push, call thunk (not a tail call), pop, call after, return the
(possibly packed) result.

**Parameters** are closures (a case-lambda over the cell `(converter . global-value)`), so
`procedure?` holds and known and unknown calls need nothing special; `RT_T_PARAMETER` is
unused. `(p)` = `(cdr (%parameter-binding cell))`: the leaf primitive returns the innermost
winders frame whose car is the cell, else the cell (deep binding). Restoring a winders list
restores every parameter, so escapes do no parameter work. `(p key)` with the private
literal `parameter-key` returns the cell; `(p v)` sets the innermost binding (converted;
R7RS leaves it unspecified). `parameterize` is `syntax-rules`:
`(parameterize ((p v) ...) body ...)` → `(parameterize-run (list p ...) (list v ...)
(lambda () body ...))`, which converts every value before pushing any frame and restores
the saved list after the thunk (no reconversion).

**Exceptions** (spec §8.3). `rt_handlers` is a list of procedures. `with-exception-handler`
pushes, calls the thunk, restores. `raise` pops the top handler and calls it with the rest
as the current stack; if it returns, `(error "exception handler returned from
non-continuable raise:" obj)` is raised in that same context; with no handler, `%uncaught`
prints "Error: msg irritants" / "Error: uncaught exception: obj" to stderr after flushing
stdout and exits 70. `raise-continuable` is the same but restores the stack and returns the
handler's value. Runtime errors: `rt_error` → `rt_raise` → generated `rt_engine_raise` → a
direct C call of `(scheme base)` `raise` (normalize.c finds the global in base's frame and
roots it in tree shaking; without `(scheme base)` it falls back to `rt_uncaught`). So
`(guard (e (#t ...)) (car (vector-ref v 0)))` catches "car: expected pair", arity errors,
"not a procedure", overflow and undefined-variable errors, with message and irritants.

`guard` is `syntax-rules` (errors.scm):

```
(guard (var clause ...) body ...)
  => (guard-run (lambda () body ...) (lambda (var) (guard-select clause ...)))
guard-select: a cond-like chain that returns #f or a thunk for the chosen clause
  ((test => f) -> (lambda () (f t)); (test) -> (lambda () t); else; (test e ...))
```

`guard-run` captures `k` with the raw `%call/cc` and installs a handler with
`with-exception-handler`. The handler runs on top of the raising frame with the guard's
handler stack and evaluates the selector: a thunk → `travel-to!` the guard's winders (after
thunks run), then `(k (guard-escape . thunk))` (`k` restores the handler stack) and
guard-run calls the thunk in the guard's context; `#f` → `(raise-continuable condition)`
from inside the handler, i.e. the outer handler in the raiser's context, its value going
back to the raiser (a `raise` then reports the secondary error). The body's normal result,
packed values included, passes through. Cost per guard entry: one `rt_call_cc` frame, one
handler-stack cons, three small closures.

**Continuations** (spec §8.2). Classification is per `call/cc` site, syntactic, after
expansion (normalize.c):
- *Tier 1*: `(call/cc (lambda (k) body))` (base's `call/cc` or
  `call-with-current-continuation`) where `k` is only called -- operator position, never
  `set!`, never referenced from inside a lambda; let-style direct applications, inlined
  `call-with-values` lambdas and nested tier-1 receivers do not count as crossing. The body
  is inlined: in value position the site is a join point with one parameter and `(k a ...)`
  is a JUMP to it (several arguments are packed); in tail position `(k a)` is a RET and the
  body keeps its tail calls. Code after a jump goes to a dead sink and is dropped
  (procedures created there are removed from `prog->lambdas`). No runtime cost; no
  dynamic-wind, parameterize or handler can come between capture and jump.
- *Tier 2*: every other site, through the library: `call/cc` =
  `(%call/cc (lambda (k) (f (escape-procedure k (%winders)))))`. `%call/cc` is
  `CTL_CALLCC` → `rt_call_cc(f)`: a `core_cont` frame on the C stack (jump buffer, saved
  winders and handlers, result) and a heap procedure `k` (`core_cont_code`) whose `free[0]`
  is the frame address (a raw word that looks like a fixnum) or `#f`. `core_conts` lists the
  live frames; a jump marks every frame it discards dead, a normal return marks its own.
  Invoking a live `k` packs its arguments (one value is itself), restores both lists and
  throws (`rt_sys_throw`; `__builtin_setjmp` on GCC and x86 clang, `sigsetjmp(b, 0)`
  otherwise, e.g. Apple clang arm64). The escape procedure checks `%continuation-live?`
  first (so a dead `k` fails before any winder runs), then `travel-to!`, then
  `(%apply k vals)`.
- *Tier 3* (M5): a dead `k` raises "re-entrant continuation not supported yet (milestone
  M5)". The hook is the dead state: M5 keeps a heap copy of the stack segment instead of
  `#f` and copies it back in `core_cont_code`; tier 2 stays as is.

**Multiple values** (spec §8.5). Calls to base's `values`, `call-with-values` (2 args) and
`apply` (2 args) are rewritten to `%values` / `%call-with-values` / `%apply` before tree
shaking. `(%values x)` is `x`; others build a values object (`rt_values_n`). A
`(%call-with-values P (lambda formals body))` has a join point whose parameters are the
formals (boxed ones get raw parameters): if `P` is `(lambda () pbody)`, each tail of pbody
(through `if`, `begin`, `letrec*`, lets) that is `(%values a ...)` jumps there directly
(no allocation; count checked at compile time); any other tail is spread with
`%values-check` + `%values-ref`/`%values-tail`. A non-literal producer is called and spread
the same way. In tail position the receiver body is a tail; otherwise it jumps to a second
join point. The variable analysis treats both lambdas as scopes of the enclosing procedure.
Unknown receivers keep `CTL_CWV` (consumer tail-called). TODO(M4): a known procedure that
returns several values could return a small C struct into `mv_spread` (comment there).

**exit**: `(travel-to! '())` then `%exit` (flushes the standard ports); statuses `#t`/none
0, `#f` 1, exact integer itself, otherwise 0. `emergency-exit` skips the unwinding.

**For the VM engineer (M3).** Re-implement only: `%call/cc` (capture the VM stack; `k`
must save and restore `rt_winders` and `rt_handlers`; re-entry works there), `%apply`,
`%values` (one value is itself, else a values object), `%call-with-values` (spread a values
object; consumer tail-called), and the raise hook (a runtime error unwinds to the dispatch
loop, which then calls the library's `raise` -- find the global as normalize.c does). For
free from `lib/`: `raise`, `raise-continuable`, `error`, `with-exception-handler`, `guard`,
`dynamic-wind`, `call/cc`'s winders handling (`escape-procedure`, `travel-to!`), parameters
and `parameterize`, `exit`; the leaf primitives (`%continuation-live?` answers `#t` for
anything that is not a compiled tier-2 escape). Tier 1 and receiver join points are
compiler optimisations; the VM runs the library definitions.

**Tests**: tests/cases values, callcc, exceptions, parameters, dynamic_wind, control_tail
(10^6 iterations through apply, call-with-values, guard bodies, dynamic-wind thunks),
higher_order, reentry, handler_returns, reraise_uncaught, exit_unwind, exit_true,
emergency_exit; tests/errors values_count, malformed_guard, malformed_parameterize. `make
test`: 123 checks. The `__builtin_setjmp` path was also checked with
`--cc "cc -arch x86_64"` (Rosetta).

**R7RS suite status** (superseded by M1 part B). `shikra check tests/r7rs/base.scm` stopped at the first identifier of
the data/ports half: `expt`, `numerator`, `denominator`, `rationalize`, `binary-port?`,
`open-input-bytevector`, `open-output-bytevector`, `input-port-open?`, `output-port-open?`,
`char-ready?`, `read-u8`, `u8-ready?`, `read-bytevector`, `read-bytevector!`, `write-u8`,
`get-output-bytevector`, `write-bytevector`, `features`. With those stubbed in a scratch
copy it builds and reports `658 tests, 621 passed, 37 failed`: every failure is a stubbed
procedure, `(string->number "#x4000000000000001/3e1")`, or the R7RS 6.10 re-entry example
(tier 3, M5).

**Known gaps**
- Bump allocation only: the 10^6 raise/catch loop allocates ~170 MB (guard closures). M2.
- Tier 2 capture costs an `rt_call_cc` frame, two closures, and a rest list per invocation.
- An after thunk that escapes during `travel-to!` abandons the rest of the original jump
  (the winders list is consistent at every step; R7RS leaves this unspecified).
- (Done in M1 part B: flushing open ports at exit, `call-with-port`, binary/bytevector ports.)

### M1 part B (the data half of `(scheme base)`: numbers, strings, bytevectors, ports)

**File map** (runtime +228 lines → 2,695 in `rt.h` + `rt_*.c`, plus 214 in `prims.def`;
compiler +5 → 4,208; Scheme library +97 → ~910)

```
src/rt_io.c     binary + bytevector ports, io_check (direction x textual/binary), port state
                predicates, read-string, char-ready?/u8-ready?, read-u8/peek-u8/read-bytevector!,
                write-u8/write-bytevector, rt_ports + rt_flush_all (flush at exit)
src/rt_str.c    str_gap (variable-width replacement), string-copy!, string-fill!
src/rt_num.c    rt_neg, rt_abs, rt_expt; string->number range rule for ratio parts
src/rt_core.c   rt_check_range (shared start/end check), bytevector-copy!
src/rt_sys.c    rt_sys_pow (libm), rt_sys_ready (poll)
src/expand.c    keyword %features: expands to the quoted feature list (library.c)
lib/scheme/base/*.scm   expt numerator denominator rationalize features call-with-port, all
                port procedures, check-range / range-error helpers (errors.scm)
tests/r7rs/expected/base.compiled   the pinned gate result
```

**Gate.** `tests/r7rs/base.scm` compiled prints `658 tests, 657 passed, 1 failed`; the failure is
the R7RS 6.10 `dynamic-wind` re-entry example (tier 3, M5), which reports `error:
"re-entrant continuation not supported yet (milestone M5)"`. `tests/run.sh r7rs` (part of
`make test`) builds each file of `r7rs_files` under each engine of `r7rs_engines` and compares
the last line plus every `FAIL:`/`ERROR:` report with its "expected"/"but got" lines against
`tests/r7rs/expected/FILE.ENGINE`, so a regression, a different error, or an unexpected pass
all fail; it also checks the generated C with `-Wall -Wextra -Werror`. M3 adds `interp` to
`r7rs_engines` (expected file `base.interp`, identical to `base.compiled`), M5 adds the other
files.

**Scheme vs. C.** C primitives are the leaf operations that need the representation or are hot:
the port machinery (every read/write primitive takes an explicit port and exact bounds),
`string-copy!`/`string-fill!` (byte surgery on UTF-8), `bytevector-copy!` (memmove),
`read-string` (buffer loop), `expt`/`abs`/negation (overflow detection with the right name).
Everything with optional arguments, defaults (`current-input-port`), procedure arguments or
folding is Scheme: `call-with-port`, `read-bytevector` (allocate, `%read-bytevector!`, trim),
`numerator`/`denominator` (scale a flonum by powers of two until integral),
`rationalize` (simplest rational between x-y and x+y, continued-fraction recursion in the
arguments' exactness), `features` (`list-copy` of `(%features)`), the list/vector procedures
with range checks (`check-range`, `range-error` in errors.scm raise the same messages as the
runtime: "who: index out of range" / "who: expected ...").

**Ports.** One `rt_port` layout for everything (rt.h). Kinds: OS descriptor (fd >= 0, 4 KB
buffer) or in-memory (`RT_PF_STRING`: the buffer is the data; an input port copies its string
or bytevector when opened, an output port grows by doubling). Orthogonally textual (UTF-8
decoded/encoded in rt_io.c) or binary (`RT_PF_BINARY`, raw bytes) -- never both; the standard
ports are textual. `io_check(port, want, who)` validates direction, kind (or `IO_ANY`, used by
`flush-output-port`) and openness, so using the wrong kind or a closed port raises "who:
expected binary input port" / "who: port is closed". Reads across the buffer end: `io_ensure`
moves the unread tail to the front and refills, so a multi-byte character or a line can span
refills (tested through stdin with `tests/cases/ports_stdin.in`, also piped in 1000-byte
writes). `read-bytevector!` loops until the range is full or end of file. `char-ready?` /
`u8-ready?`: true when bytes are buffered, for in-memory ports, or when `poll` says a read will
not block (data or end of file). OS output ports other than stdout/stderr are kept on
`rt_ports` (a GC root, unlinked by `close-port`); `rt_exit` (normal end, `exit`,
`emergency-exit`, uncaught error) flushes stdout, then every port on that list, then stderr.
`close-input-port`/`close-output-port` check the direction; closing twice is harmless.

**String mutation** (spec 3.2). Strings keep `{chars, bytes, buf}` with `RT_F_ASCII` meaning
`chars == bytes`. ASCII strings index in O(1) everywhere (`str_offset` returns k). Non-ASCII
strings find a byte offset by a linear scan from the start. `str_gap(s, da, db, n)` replaces the
byte range [da, db) with room for n bytes: in place when the size is equal, else into a fresh
buffer (old buffer untouched, so a source inside the same string stays valid), recomputing
the ASCII flag. `string-set!`, `string-copy!` and `string-fill!` all go through it, so each
call moves the string's bytes at most once (a non-ASCII `string-copy!` of n characters is
O(length), not n calls of `string-set!`), and a string that becomes ASCII again regains the
fast path.

**Numbers.** `expt`: exact base and exponent >= 0 by squaring with overflow checks ("expt:
fixnum overflow"); a negative exact exponent gives 1/b^-e as a flonum (exact for bases 1 and
-1; `(expt 0 -1)` raises), the same rule as `/`; anything inexact goes to `rt_sys_pow`. Unary
`-` and `abs` are primitives so that the most negative fixnum raises an overflow naming them,
and `(- 0.0)` is `-0.0`. `string->number` returns `#f` (never raises) for any text that is not
a number Shikra can represent: an exact integer outside the fixnum range, a ratio with either
part outside it (`"#x4000000000000001/3e1"`), `#e` of a non-integer, complex syntax. A ratio
whose parts fit is exact when it divides, else a flonum (as the reader, M0). Fixnum/flonum
comparison is exact (M0, `num_cmp_id`). `max`/`min` check their arguments and are inexact
if any argument is.

**What M6 must supply for math** (no libc): `rt_sys_pow` (IEEE `pow`, needed by `expt` on
flonums) -- a correctly rounded or at least faithful `pow` (exp2/log2 based), and `poll` for
`rt_sys_ready` as a system call. Rounding (`floor` `ceiling` `round` `truncate`) and integer
square roots are done without libm already. M5's `(scheme inexact)` will add `exp log sin cos
tan asin acos atan sqrt` to the same platform layer.

**Decisions / deviations**
- *Error names inside library procedures*: when a library procedure delegates to a primitive
  of another name, the error names the primitive: `string-copy`, `write-string` and
  `string->utf8` ranges report `substring`, char comparisons report `char->integer`,
  `string->vector`/`vector->string` report `string->list`/`list->string`, `floor-remainder`
  reports `modulo`, `truncate-*` report `quotient`/`remainder`, `utf8->string` ranges report
  `bytevector-copy`, `square`/`lcm` overflow reports `*`, defaults computed with
  `string-length`/`bytevector-length` report those on a wrong-type argument. The message still
  names a real procedure the call reached; fixing it needs a `who` argument on the shared
  primitives (M4 could pass it as a constant).
- *Ports are textual or binary, not both*; the standard ports are textual (R7RS allows either).
- *`char-ready?` with a partial UTF-8 sequence buffered* may answer `#t` and then block until
  the rest of the character arrives.
- *`numerator`/`denominator` of an infinity or NaN raise* ("expected rational number"); R7RS
  leaves them undefined. `(expt x y)` with a negative flonum base and non-integral exponent
  is `+nan.0` (no complex numbers).
- *`number->string` of a flonum in radix 2, 8 or 16 raises* (R7RS allows an error for inexact
  numbers in other radices).
- *Flonum output*: positional notation for 1e-7 <= |x| < 1e21 (`0.0000001`,
  `100000000000000000000.0`), exponent notation otherwise (`1e21`, `1.5e-10`); digits are the
  interim shortest-round-trip of M0 (M5).
- *`rt_ports` keeps unclosed file ports reachable* until exit (they must be flushed); a
  program that opens many file ports without closing them keeps their buffers. M2 may make
  the list weak if that matters.

**Tests** (tests/cases, each self-checking through `(util check)` unless noted):
`strings_utf8` (width-changing `string-set!`, `string-copy!` both directions and overlapping,
`string-fill!`, ASCII fast path regained), `ports_memory` (byte round trips, binary reads at
end of file, kinds and states, `call-with-port`, errors), `ports_stdin` (+ `.in`: reads across
the 4 KB buffer; output compared with values computed independently), `numbers_edges`
(overflow errors of `+ - * abs quotient / expt exact`, `exact-integer-sqrt`, flonum printing
of `1.0 1e21 -0.0 +inf.0 +nan.0` ..., exact mixed comparison), `string_to_number`
(rejections), `error_names` (64 error situations); rt_unit: `rt_ports` bookkeeping and the
flush at exit (run.sh checks the file afterwards). `make test`: 138 checks.

**Known gaps**
- `string-ref` on non-ASCII strings is O(n) per call, so an index loop over such a string is
  quadratic (spec 3.2 allows it); `string-for-each`/`string->list` are linear. A cached
  (index, offset) cursor would fix the common sequential case.
- `utf8->string` of invalid UTF-8 substitutes U+FFFD instead of raising.
- `(scheme file)` (and so `open-binary-input-file` etc.), `read`, `write-shared` are M5;
  file ports already exist in the runtime and are flushed at exit.
