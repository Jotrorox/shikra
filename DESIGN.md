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
- Stress mode: collect at every allocation (a build made with `--gc-stress`, a hidden
  testing flag; env `SHIKRA_GC_STRESS=N` collects at every N-th allocation instead, 0 off).
- In the tool: compile commands run with collection disabled (batch program,
  frees nothing). In the REPL, front-end allocations (datums, expander state,
  constants) are made in *immortal* mode: never reclaimed. (M3: front-end data uses the
  unscanned "pure" immortal mode; mutable immortal objects -- global cells, copies of data
  handed to `eval` -- the scanned one. See "M3 as built".)

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
  the optimizations of spec §5 pass 6. M4-1 built the structural passes (opt.c; "M4-1 as
  built"), M4-2 the analysis, check elimination, specialisation and unboxing (analyze.c, emit.c;
  "M4-2 as built").
- **Emit**: one C file (see §3), then `cc`. Until M4 the backend is naive:
  every procedure has the uniform signature and every primitive is checked.
- Closed world: a top-level definition never `set!` is a constant; `set!` on an
  imported binding is a compile error.

## 8. Interpreter (M3)

Front end → core forms → compact bytecode → VM with its own value stack (no C
recursion for Scheme calls; tail calls in constant space; continuations copy
the VM stack). Open top level (globals in a table, redefinable). Always
checked. Uses the same runtime and the same Scheme library sources as compiled
programs. Never linked into compiled programs. As built: "M3 as built" in §11.

## 9. Tests

```
tests/run.sh [compiled|r7rs|interp|stress|expand|all]   driver used by `make test`
tests/cases/NAME.scm + NAME.out             program + expected stdout (+ NAME.exit,
                                            + NAME.err: expected stderr substring,
                                            + NAME.in: stdin, else /dev/null,
                                            + NAME.flags: extra shikra flags (M5),
                                            + NAME.args: program arguments (M5),
                                            + NAME.stress-interval: see M2 "Stress mode",
                                            + NAME.interp-stress-interval: M3 "Stress mode",
                                            + NAME.compiled-only: reason the interpreter
                                              legitimately differs; reported as skipped)
tests/repl/NAME.in + NAME.out (+ NAME.args) input piped into `shikra repl`, whole output
tests/errors/NAME.scm + NAME.err            expected compile diagnostics (substring)
tests/expand/NAME.scm + NAME.out            core forms as `,expand` prints them
tests/r7rs/                                 adapted Chibi r7rs-tests, split by library
tests/r7rs/expected/FILE.ENGINE             pinned summary line + failing tests (r7rs mode;
                                            since M5 every file under both engines)
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
- (Resolved in M2: quoted pairs are read-only too. All literals are `static const`; literal
  pairs sit in one array and `set-car!`/`set-cdr!` raise "expected mutable pair" for them.
  In the interpreter (M3) quoted pairs are immortal heap pairs, and `RT_LITPAIR` in tool mode
  asks the collector whether a pair is immortal, so mutation is detected there too.)
- (Resolved in M4-1: tree shaking covers user code. All user code is still normalized, so the
  middle end's diagnostics cover it, and the optimizer then drops what nothing reachable uses;
  unused user definitions and imports are warnings.)
- (Resolved in M4-1: the trampoline fallback exists, `RT_TAIL`/`RT_NT` in rt.h, forced with the
  hidden `--no-musttail`.)
- *The receiver of a tier-2 `call/cc` is not called in tail position* (R7RS 3.5 requires
  it; M1 part A): `rt_call_cc`'s C frame holds the jump target while the receiver runs, so a
  loop that recurses through `call/cc` grows the C stack. Tier-1 sites in tail position are
  proper (the receiver body is inlined). (M5: tier 3 copies the C stack, so `rt_call_cc3` keeps
  its frame too; this stays a deviation for tiers 2 and 3. Fixing it needs heap-allocated frames,
  i.e. CPS conversion, the spec's §15 fallback.)
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
- (Resolved in M4-2: hello world on macOS arm64 is 50,880 bytes at -O2, `__text` 1,568 bytes -- the
  spec's ~50 KB format floor. The port parameters are static cells, `display` of a string to the
  proven standard output is a buffered write, nothing allocates and nothing can raise, so neither
  the collector nor `raise` is linked. Before: 70 KB since M1, 69,920 after M4-1.)
- *The REPL echoes its input when stdin is not a terminal* (M3; spec §11.2 shows a terminal
  session): piped input then produces a transcript identical to the spec's session, which is
  what tests/repl compares.
- *An uncaught error at the REPL prompt does not run pending `dynamic-wind` after thunks*
  (M3): the session's winders and handler lists are reset to empty (so parameterize bindings
  are undone) and the prompt returns; running the after thunks could raise again.
- *Redefining an imported name at the REPL shadows it for session code only* (M3): library
  code keeps calling the library's own binding (it was resolved when the library was
  expanded); callers inside the session see every new definition.
- *Interpreter stack overflow is not catchable* (M3): beyond 2^25 slots (256 MB) the VM
  prints "Error: stack overflow" and returns to the prompt (`shikra interp`: exits 70);
  there is no room left to run a handler. Compiled programs crash on C stack overflow.
- *M5 libraries* (details in "M5 libraries as built"): no complex numbers, so `sqrt`, `log`,
  `asin`, `acos` of a real outside the function's real domain return `+nan.0` (R7RS: a complex
  result); `current-second` is POSIX time (UTC), not TAI; `force` of a non-promise returns it;
  `string-downcase`'s final sigma looks at the neighbouring characters only (no skipping of
  case-ignorable characters) and the language-specific mappings (Turkish, Lithuanian) are not
  applied; a ratio whose denominator has more than 780 significant digits is not a number
  Shikra can represent (`string->number` gives `#f`).
- *Generic `display` and the uncaught-error printer link the datum-label pre-pass* (§9.1:
  hello world never prints a cycle): both must terminate on circular data. (M4-2: only when they
  are reachable on a value whose type may be a pair or a non-empty vector; `display` of a proven
  string, char, fixnum or other non-compound value uses its own writer, and a program with no
  exception handler reports errors without the generic printer when the irritants are fixnums.)
- *A provably wrong type is a compile error only when the proof is local* (§11.3 lists `(car 5)`
  as an error; M4-2): when the value reaches the primitive through a user procedure's parameter or
  result, or the code is in a guard body or a procedure passed to other code (which may run it
  under an exception handler, as `test-error` does), or behind an undecided conditional, it is a
  warning and the code raises at run time, so no valid program is rejected. Likewise an arity
  mismatch at a call only the analysis made known is a warning (syntactically known calls remain
  errors, M0).
- *Stress mode collects at every allocation for all but five cases* (§13 M2 "done when"):
  the r7rs gate and 36 of the 41 programs in tests/cases collect at every allocation, at
  -O2 and -O0; five whose live set or allocation count makes a full collection per
  allocation quadratic use a per-case interval (M2 "Stress mode" lists them and why).
- *Tier 3 is not available under hardware shadow stacks* (spec §15 risk; M5): copying the C stack
  back invalidates the return addresses a shadow stack (Intel CET, Arm GCS) has recorded, so a
  re-entry would fault. Programs must be built without shadow-stack enforcement; the CPS fallback
  the spec names is not implemented. Tiers 1 and 2 are unaffected (setjmp/longjmp is supported).
- *The tier-2 escape analysis ignores run-time error irritants* (M5, "Tier 3 continuations as
  built"): a continuation can also leave its extent as the irritant of a type error the program
  commits (`(vector-ref k 0)`, calling a list that holds `k`), caught by a handler that keeps the
  condition. Those are "it is an error" situations; calling such a `k` later raises the safe-mode
  error "tier-2 continuation called after its extent (bug)" -- nothing is corrupted.

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
`raise`; may fall back to `rt_uncaught(e)`). (M2: static symbols are listed in
`rt_static_syms[]`; `rt_intern_static` is gone.) Static literals use
the RT_HDR macro and the structs in rt.h; set `RT_F_RO` on literal strings, vectors
and bytevectors (mutators check it). (M2: everything static is `const`, pairs go into
`rt_lit_pairs[]` -- see "M2 as built", "Static data".)

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
cell is an object start; (Since M2 the fast path records
nothing; see "M2 as built" for the slow path, roots, immortal mode and collection.) Rule: initialise every field of a
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
- (Done in M5: shortest float printing, correctly rounded parsing; `equal?`, `write` and
  `display` terminate on circular data. See "M5 libraries as built".)
- (Fixed in M1 part B: every open output port is flushed at exit.)
- `exact`/integer division on flonums outside ±2^62 raise an error.
- (Done in M2: the Immix collector, `--gc`, immortal mode.)
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
(`RT_ARGBUF` = 64 static slots registered as GC roots; `rt_args_reserve` grows it and
registers the new buffer as a root range), stored right before the call, copied out in the
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
RT_GC_BUMP RT_UNICODE_FULL RT_GC_STRESS`, from the flags; since M2 the runtime reads
`RT_GC_BUMP`, `RT_GC_STRESS` and `RT_UNSAFE` (literal-pair check), plus `RT_NLITPAIRS` and
`RT_NSTATIC_SYMS`) + rt.h + program + `rt_sys.c rt_gc.c rt_core.c rt_str.c rt_num.c rt_io.c rt_read.c`.
Program: literals as static data (`L<n>`: `rt_string`/`rt_flonum` (hex float)/
vector/bytevector structs, `RT_F_RO` on strings, vectors, bytevectors; symbols are
static `rt_symbol`s listed in `rt_static_syms`; since M2 everything is `static const` and
pairs are elements of `rt_lit_pairs[]`), `rt_g[]` (non-procedure globals, initialised to `RT_UNDEF`, one root range),
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
- (M4-1 removed most of the following; see "M4-1 as built".) M4 (performance): every variable is a C local of type `obj`, every primitive is the
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
- *Tier 3* (M5, done: "Tier 3 continuations as built" at the end): a site whose `k` may
  outlive its extent calls `rt_call_cc3`, which also copies the stack; invoking its dead `k`
  re-enters. A dead tier-2 `k` (a classification bug) raises "tier-2 continuation called after
  its extent (bug)". In compiled code the escape procedure no longer travels
  (`%continuation-live?` is #f there: compiled continuations walk the winders themselves).

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
- (Done in M2: the 10^6 raise/catch loop of exceptions.scm peaks at 2.6 MB RSS instead of
  130 MB under bump.)
- (M4-1: a tier-2 capture now costs the `rt_call_cc` frame on the C stack and a 3-word `k`; a
  literal receiver lives on the C stack. See "M4-1 as built", continuations.)
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
all fail; it also checks the generated C with `-Wall -Wextra -Werror`. (M3 replaced the two
lists by `r7rs_runs`, FILE:ENGINE pairs, and added `base:interp` -- 658/658, since the
interpreter's continuations are re-entrant -- and `eval:interp`.) M5 adds the other files.

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

### M2 (the collector, static literals and symbols)

**File map** (runtime +490 lines → 3,185 in `rt.h` + `rt_*.c`; `rt_gc.c` 496 lines, of which
the Immix collector is ~430; compiler +10)

```
src/rt_gc.c      shared: roots registry, enable flag, rt_alloc_raw; RT_GC_BUMP: the never-freeing
                 allocator; default: Immix (heap, spaces, marking, collection, allocation)
src/rt.h         memory API (+ rt_gc_enable), rt_lit_pairs[] / RT_LITPAIR, rt_static_syms[]
src/rt_core.c    rt_init allocates nothing; symbol table made on first use; rooted rt_args growth
src/rt_io.c      static standard ports
src/rt_sys.c     rt_sys_free (munmap), rt_sys_getenv
src/emit.c       static const literals and closures, rt_lit_pairs[], RT_NLITPAIRS/RT_NSTATIC_SYMS
tests/cases/gc_*.scm, literals.scm, *.stress-interval; tests/run.sh stress; rt_unit.c GC tests
```

**Heap layout.** Memory comes from `rt_sys_mem` in *chunks* of 32 KB-aligned *blocks* (first
chunk 32 blocks = 1 MB; each growth adds a chunk as large as the whole block heap, so there are
few chunks: at most 48). Each chunk has a side array of block descriptors
`{kind, line[128], start[64], mark[64]}` (1.2 KB per 32 KB block): kind is free, headered or
pairs, optionally immortal; one state byte per 256-byte line (`L_FREE`, `L_LIVE` = held a
marked object at the last collection, `L_NEW` = claimed since, `L_RUN` = first line of a
claimed headered run, plus the `L_MARK` bit during marking); start and mark bitmaps with one
bit per 8-byte granule. A pair block holds only pairs, so every 16-byte cell is an object start
and needs no start bit. Requests above 8 KB that miss the fast path go to the *large object
space*: one mapping each (`{size, bytes, mark, imm}` header, object at +32), kept in a sorted
table for interior lookups, unmapped when dead. (Objects up to 32 KB can still land in a block
when they fit the current run.)

**Allocation.** Five spaces bump through runs of free lines: headered and pair streams (their
cursor/limit are the fast-path globals `rt_hp/rt_hlim`, `rt_pp/rt_plim`), an *overflow* space
for medium objects (> 256 bytes) that miss the current headered run, and immortal headered and
pair spaces. The slow path (`rt_alloc_slow`, `gc_refill`) searches, in address order from where
the space stopped, the next block that is free or (for the two streams) of its kind, claims the
next maximal run of free lines, and continues; overflow and immortal spaces take whole free
blocks only (immortal ones zeroed). When no block is left: collect if collection is enabled and
lines were claimed since the last collection, else add a chunk. The fast path stays add,
compare, store and records nothing.

**Object starts.** Claiming a headered run marks its first line `L_RUN`. Whenever a space leaves
a run, and at the start of each collection for the current runs, a zero word is written at the
cursor (headers are never zero). A collection first walks every `L_RUN` run by header sizes up
to the zero word or the run end and sets the start bits; sweeping keeps the start bits of marked
objects only, so survivors keep theirs and dead objects lose theirs.

**Marking.** Roots, in order: registered ranges (precise), immortal objects (every object in an
immortal block or large immortal object is scanned as a root, and marking never enters them), then
the C stack from a local of the collector up to the base given to `rt_init`, word by word. The
collector runs inside `rt_sys_catch`, whose jump buffer (sigsetjmp) or frame (`__builtin_setjmp`
makes the compiler spill callee-saved registers) holds the register contents, so registers are
covered. Two marking entries:
- `gc_mark(x)`, *precise* (root ranges, heap fields): only a tagged word is followed; the target
  must be a valid object: in a pair block a 16-byte-aligned cell in a non-free line (tag 001), in
  a headered block a set start bit (tag 101, or 011 with the header one word before the code
  word), or exactly a large object. Anything else (static data, stale words) is ignored.
- `gc_mark_any(a)`, *conservative* (stack words, the raw `buf` of strings and ports): a keeps the
  object it points at or into alive, tagged or not: in a pair block the cell containing a (non-free
  line), in a headered block the object whose start is the nearest set start bit at or below a if
  a lies inside it, or a large object containing a.

Marking an object sets its mark bit and the `L_MARK` bit of every line it covers, then pushes its
obj fields as one range on an explicit mark stack (`gc_scan`): header types 1..15 all words after
the header, procedures the captured values (not the code word), ports the name; string and port
buffers are marked on the spot (raw, no fields); raw types 16..31 nothing. `gc_drain` pops a
range, re-pushes everything beyond 64 fields (long vectors in slices) and marks the slice's
fields last-to-first, so the first field is popped first (lists of lists stay shallow). The mark
stack grows by doubling up to 2^20 entries (2^12 in stress builds); when it cannot grow, the
object stays marked but unscanned and `gc_rescan` later rescans every marked object, in
descending address order (objects usually point to older, lower ones), until a pass overflows no
more. Deep structures never recurse on the C stack.

**Sweep and growth.** A line is free iff it holds no marked object; a block with no live line
becomes free (for either kind); large objects not marked are unmapped. Every space restarts its
search at the first block. If more than half of the block heap is still in use, a chunk the size
of the heap is added (the heap doubles). Large-object allocation collects first once more bytes
than max(live large bytes, block heap size, 4 MB) were allocated since the last collection. The
heap never shrinks (free blocks are reused, not returned to the OS); there is no defragmentation
(non-moving; spec §15 risk).

**Enable flag, tool mode.** Collection is on in programs and off in the tool
(`rt_gc_enable(1)` turns it on: M3's REPL); compile commands therefore allocate through the same
blocks without ever collecting (their datums live in compiler arenas the collector cannot see).
`rt_gc_collect()` collects at once (no-op before the first allocation or under bump).

**Immortal mode.** `rt_gc_set_immortal(1)` switches the fast-path globals to the immortal spaces;
allocations then go to zeroed immortal blocks (or immortal large objects) that are never swept or
reused and whose objects are scanned as roots at every collection, so mortal objects they point to
survive. `rt_gc_set_immortal(0)` switches back. Tested in rt_unit.c.

**Static data** (spec §6.1, §7). The emitter writes every literal and every closure-free
procedure as `static const` (read-only data; `__DATA_CONST` on macOS) with no startup code:
strings, vectors, bytevectors carry `RT_F_RO`; flonums, symbols (a static name string) and
closures are plain const structs; *all pairs* are elements of one array
`static const rt_pair rt_lit_pairs[RT_NLITPAIRS]` (declared tentatively by rt.h, so other
literals may point into it), and `set-car!`/`set-cdr!` check `RT_LITPAIR(x)` (one subtraction,
one comparison against a link-time constant) and raise "set-car!: expected mutable pair"; with
`--unsafe` the check is compiled out and a write faults (SIGBUS) because the data are read-only.
The collector never sees static data (outside the heap). Symbols in source are static and
compared by address; they are listed in `static const obj rt_static_syms[RT_NSTATIC_SYMS]`, which
only the symbol table's creation reads: the table is made by the first `string->symbol` (or
`read`), seeded with them, so `string->symbol` returns the static symbol and a program that never
interns links neither the table nor the list. `symbol->string` needs no table.

**Pay for use** (spec §9.1). `rt_init` allocates nothing (the standard ports are static structs
with static 4 KB buffers; no symbol table) and only records the stack base and root ranges.
Everything of the collector hangs off `rt_alloc_slow`, so a program that never allocates drops it
at link time: a program-mode stand-in that only writes a string has the same 1,016 bytes of text
with immix and with `RT_GC_BUMP`. In practice every Scheme program allocates (the library's
parameters), so it links the collector: hello world is 70,128 bytes (text 21,040) with immix,
69,312 (text 14,328) with `--gc=bump`; before M2 it was 69,192 (text 15,736).

**Root audit.** Registered ranges: `rt_g[]` (program globals), `core_symtab`, `core_argbuf`
and every grown `rt_args` buffer (an abandoned buffer is cleared), `rt_winders`, `rt_handlers`,
`rt_stdin/out/err`, `rt_ports`. On the C stack (conservative): `core_cont` frames of tier-2
`call/cc` (jump buffer, saved lists, `k`), the reader's `rd_t`, `rt_values_n` argument arrays,
everything of generated code. `core_conts` points to stack frames only; a continuation's
`free[0]` is a raw frame address that looks like a fixnum. Tool-only: the reader's source
location table (OS memory, unscanned: datums must be immortal or collection off).

**For the VM engineer (M3).** The VM must make its roots visible: register the value stack once
as a fixed array with `rt_gc_add_roots` (keep the slots above the stack pointer holding valid objs
or fixnum-looking words, e.g. clear them on pop or when a frame is abandoned; at most 64 ranges in
all), likewise the globals table if it lives in C memory (or allocate it on the heap and root one
word), and any C-side caches of objs. Values in C locals of the VM loop are covered by the stack
scan. Call `rt_gc_enable(1)` once the REPL starts; allocate front-end datums, macro environments
and bytecode constants with `rt_gc_set_immortal(1)` around the front end (the source-location
table holds pair addresses the collector does not see), then switch back before running user
code. Error recovery that longjmps to the prompt is fine: nothing is held across.

**For tier 3 (M5).** A heap copy of a C stack segment must be an object the collector scans
*conservatively*: give it its own header type (e.g. `RT_T_CONT`, today in the raw class) and add a
case to `gc_scan` that marks every word of the copy with `gc_mark_any` -- through the mark stack,
with a "conservative" flag on the range (e.g. the low bit of `lo`) so that chains of
continuations do not recurse. The copied jump buffer then also covers the saved registers. While
a segment is copied back onto the C stack it is ordinary stack and needs nothing. Keep the
original pointers valid: the collector is non-moving, so addresses in the copy stay correct.

**Stress mode.** `--gc-stress` defines `RT_GC_STRESS`: the fast-path limits are kept equal to the
cursors, so every allocation enters the slow path, which collects at every allocation, or every
N-th with env `SHIKRA_GC_STRESS=N` (0: off), and the mark stack is capped at 4,096 entries so
that the overflow rescan runs too. `tests/run.sh stress` (part of `make test`) builds every case
and the r7rs gate at -O2 and at -O0 and compares with the normal expected results (r7rs: the same
`tests/r7rs/expected/base.compiled`). The -O0 pass raises the C stack to 64 MB (`ulimit -s`):
callcc.scm recurses 10^5 frames deep and -O0 frames exceed 83 bytes; that program overflowed the
default 8 MB stack at -O0 before M2 as well. Per-case intervals (`NAME.stress-interval`), each
because a full collection per allocation would be quadratic:

| case | N | reason |
|---|---|---|
| tail_calls | 1,000 | 2·10^7 allocations, up to 10^6 closures live (the `cps` chain) |
| gc_cons | 100 | 1.2·10^7 allocations |
| gc_mixed | 100 | ~5·10^6 allocations |
| gc_reach | 10 | ~2·10^6 allocations (34 s at N = 1) |
| gc_deep | 100,000 | ~5·10^6 allocations with 3·10^6 objects live |

(In the gc_deep stress run the 4,096-entry mark stack overflows and the rescan runs at most
collections.) gc_large runs at N = 1. All other cases and the r7rs gate run at N = 1: the gate makes 10,912
collections. Timing (Apple M-series, this machine): `tests/run.sh stress` 2:52 for 84 checks,
`make test` 3:14 for 234. The register-capture path through `__builtin_setjmp` was also checked
under stress with `--cc "cc -arch x86_64"` (Rosetta).

**Memory** (peak RSS, `/usr/bin/time -l`, immix vs `--gc=bump`): gc_cons 2.6 MB vs 194 MB,
gc_mixed 2.6 MB vs 387 MB, gc_large 7.8 MB vs 502 MB, gc_reach 2.6 MB vs 117 MB, the 10^6
raise/catch loop (exceptions.scm) 2.6 MB vs 130 MB, tail_calls 26 MB vs 42 MB (it keeps 10^6
closures live), gc_deep 157 MB vs 145 MB (80 MB live plus a 16 MB mark stack). Run times equal
within noise.

**Bugs stress mode exposed in existing code**: none. Every case and the gate passed under
stress on the first run at -O2; the only -O0 failure (callcc.scm) is the stack depth above, not
a collector issue. The audit before that found two latent problems, fixed: the grown argument
buffer was not a root, and `rt_init` allocated (ports, symbol table) for every program.

**Decisions**
- Object starts by walking new runs at collection time instead of a store in the fast path.
- Headered heap fields are trusted only at a start bit; untagged string/port buffers and stack
  words are looked up conservatively. A stale or dead object kept by a conservative pointer can
  keep what it points to alive (validated, never misread).
- `rt_ports` stays a strong list: an unclosed file port must be flushed at exit.
- Interned symbols are never collected (the symbol table is a strong root).
- The standard ports cost 12 KB of zero-fill (`__bss`), not file size.

**Known gaps**
- No compaction or block release to the OS; fragmentation under long runs is untested (spec §15).
- The mark-stack rescan is quadratic for deep structures built top-down beyond 2^20 pending
  entries (never seen in the suite).
- Quoted data in the REPL (immortal heap pairs) are mutable (no `RT_LITPAIR` there).
- Fixed limits: 64 root ranges, 48 chunks (with doubling growth: far beyond any real heap).
- Weak references, finalizers, guardians: none (R7RS-small needs none).

### M3 (the interpreter and the REPL)

**File map** (interpreter 1,480 lines of C: `interp.h` 93, `bytecode.c` 468, `vm.c` 522,
`repl.c` 396; runtime +40, front end +40)

```
src/interp.h     templates, the instruction set, the API between the three files
src/bytecode.c   core forms -> bytecode: variable analysis, closures, letrec*, units
src/vm.c         the VM: stack, dispatch loop, continuations, errors, eval/environment
src/repl.c       shikra repl (input, printing, ,expand, ,c, ,quit) and shikra interp
src/main.c       repl and the hidden command `interp FILE [args]`
lib/scheme/{eval,load,repl}.sld   REPL-only libraries on %eval %environment
                                  %interaction-environment (+ %read for load)
src/rt_gc.c      dynamic root ranges, pure immortal mode, tool stress, rt_gc_immortal_pair
src/library.c    front_opts.repl gates the REPL-only libraries; cerror_hook; front_repl_recover
```

**Pipeline.** Front end unchanged (passes 1-3) → core forms → `bc_unit` → a *template* per
lambda clause → `vm_run`. A *unit* is the top-level code compiled at once: the bodies of every
library loaded since the last unit (front_order after the last compiled one), then the given
top-level forms; it is a zero-argument template whose code ends with RET. `shikra interp`
compiles the whole program with its libraries as one unit (so a top-level continuation
includes the rest of the program, as in a compiled program); the REPL makes one unit per entry
(including the libraries an `import` just loaded). The interpreter never calls `ir_build` or
the emitter.

**Templates and closures.** `tmpl` (C memory, never freed): code (`int32_t[]`), constants
(the elements of an immortal vector), name (for arity errors), nreq, rest, nlocals, maxstack,
nfree, case-lambda clauses. A closure is an ordinary procedure object (`rt_make_proc` with code
`vm_closure_code`): `free[0]` = the template's address (8-aligned, a fixnum to the collector),
`free[1..nfree]` = captured values. A case-lambda closure's template lists clause templates
that share one free-variable layout; the VM picks the first clause whose arity fits. A
continuation is a procedure with code `vm_cont_code` whose `free[0]` is its stack copy. Both
code functions raise if ever called from C (nothing in the tool calls Scheme from C).

**Variables.** Locals are unique uninterned symbols, so a symbol occurrence is a reference to
its binder (no scopes needed). Each binder gets a frame slot of its procedure; a reference
from an inner lambda is a free variable of every lambda in between (flat closures, captured at
closure creation). *Boxed* (slot holds a box): every assigned variable -- not only captured
ones, because continuations copy the stack and a copy must share the location -- and every
*early* letrec* variable: one mentioned by the init of its own or an earlier binding, except
when both inits are lambdas of the same consecutive lambda run. Reads of early variables are
checked ("x: variable used before its definition"). Lambda runs are created in order; captures
of not-yet-created run members are placeholders patched by OP_FIXCL afterwards. Assigned
parameters are boxed on entry. `((lambda (v ...) body) arg ...)` with matching counts is a
let (slots, no closure). Lambdas are named as the compiled engine names them (global name,
let/letrec variable name unless assigned, else `lambda`), so arity errors read the same.

**Globals.** One *cell* per front-end `global` (`global.cell`): an immortal (scanned) vector
`[value, name, defined?]`; GREF/GSET/GDEF reference it as a constant, so redefinition at the
prompt is seen by every caller. `defined?` is set when a unit containing a `define` of it is
compiled: reading the undefined marker then reports "x: variable used before its definition"
(the compiled wording), otherwise "x: unbound variable" (REPL only; the open top level makes
unbound identifiers globals). As in compiled programs, a top-level procedure defined once and
never assigned exists from the start of its unit (DESIGN §10, "known top-level procedures").

**Instruction set** (interp.h has the authoritative list with operands): CONST; LREF LSET FREF;
LBREF FBREF LBREFC FBREFC LBSET FBSET BOX MKBOX (boxes); GREF GSET GDEF; PUSH; JMP JMPF;
CLOSURE FIXCL; FRAME CALL TCALL RET; PRIM (p, n: the first n-1 arguments pushed, the last in
acc); VALUES; SPREADL SPREADV CALLV TCALLV CWVSWAP VCHECK (apply and call-with-values);
CALLCC TCALLCC; EVAL TEVAL ENV INTERENV; HALT. Accumulator machine: every expression leaves
its value in `acc`.

**Frames** (fp points at the first argument):

```
fp[-4] caller's fp (fixnum: stack index)    fp[-3] caller's pc (fixnum: code offset)
fp[-2] caller's template (raw, 8-aligned)   fp[-1] the running procedure (its closure)
fp[0 .. nlocals)  parameters, then let/letrec* variables     above: temporaries
```

A non-tail call: FRAME (3 placeholder slots), push f and the arguments, CALL n fills in the
return information and enters. TCALL moves f and the arguments down to fp[-1] (constant
space). Entry checks the procedure, picks a clause, checks arity, makes room for nlocals +
maxstack + 16 slots (the stack is one `realloc`ed array, doubling up to 2^25 slots), builds the
rest list, sets the remaining locals to the undefined marker. RET pops the frame. The bottom
frame returns to a one-instruction HALT template. Every slot below sp is an obj or a
fixnum-looking word, so the stack is a precise root range.

**Continuations.** CALLCC copies the stack below its frame into a vector `[fp, pc, template,
winders, handlers, slots...]`; TCALLCC copies below the current frame and uses the frame's
return information (constant space for loops through `call/cc` in tail position). Invoking it
copies the slots back, restores `rt_winders`/`rt_handlers` and continues with the argument
(several arguments: a values object). Fully re-entrant: generators, re-entering
`dynamic-wind` (the library's escape procedure rewinds first; `%continuation-live?` is #t for
VM continuations), re-entering top-level forms. Cost: proportional to stack depth per capture
and per invocation.

**Primitives** are called through a function-pointer table built from prims.def
(`prims[]`, cast to the exact arity, 0..5). `%uncaught` is replaced by the VM's (REPL: print,
return to the prompt). The control operations are instructions: `%apply` = SPREADL+CALLV,
`%call-with-values` = CWVSWAP, CALL 0, SPREADV, CALLV (with a literal lambda receiver, VCHECK
first: "call-with-values: wrong number of values for the receiver", as compiled), `%values`
= VALUES, `%call/cc` = CALLCC.

**Errors.** Before anything that can raise or allocate, the loop saves its registers (`SYNC`).
`rt_raise_hook` (set during `vm_run`) longjmps (`rt_sys_throw`) back to `vm_run`, which pushes
a call of `(scheme base)`'s `raise` with the error object on top of the faulting frame (its
return point is after the faulting instruction) and re-enters the loop -- so `guard`,
`with-exception-handler` and `raise-continuable` are the library's, exactly as compiled. With
no `raise` defined yet, or when `raise` reaches `%uncaught`: `shikra interp` prints and exits
70 like a program, the REPL prints and returns to the prompt.

**Memory and roots.** Collection is on in the interpreter (`rt_gc_enable(1)` in `vm_init`);
compile commands are unchanged. Front-end work (reading, expansion, bytecode, templates'
constants) allocates in *pure immortal* mode (`rt_gc_set_immortal(2)`): never reclaimed and
not scanned, which is valid because such objects point only to immortal objects, static data
and interned symbols (the symbol table is a root). Objects that are mutated later or may hold
collectable values are *scanned immortal* (mode 1): global cells and the copies of data given
to `eval`/`environment` (they may contain procedures). Literal strings, vectors and
bytevectors get `RT_F_RO`; literal pairs are immortal, and tool-mode `RT_LITPAIR` asks
`rt_gc_immortal_pair`, so mutating a literal raises as in compiled code. Roots: the VM stack
as a *dynamic* range `[stk, vm_sp)` (`rt_gc_add_dynamic_roots`, bounds read at each
collection), `vm_err`, the REPL's record of definitions; registers and C locals of the loop are
covered by the conservative stack scan. Source locations are recorded only during front-end
work (the table holds addresses the collector does not see). Unscanned immortal data made a
collection at every allocation cheap (r7rs base: 11.5 s with scanned front-end data, 0.09 s).

**Stress mode.** The tool reads `SHIKRA_GC_STRESS=N` (default 0; programs: as in M2) and then
collects every N-th allocation once collection is enabled (not during immortal allocation).
`tests/run.sh stress` runs every case and r7rs base through `shikra interp` at N = 1, except
(`NAME.interp-stress-interval`; each takes 2.5-6 s at that interval, 40 s to minutes at 1):

| case | N | | case | N |
|---|---|---|---|---|
| control_tail | 10 | | gc_mixed | 100 |
| exceptions | 10 | | gc_reach | 10 |
| tail_calls | 1,000 | | gc_deep | 100,000 |
| gc_cons | 100 | | | |

Checked that it bites: with the VM stack root removed, r7rs base and closures.scm fail under
stress and pass without it.

**The REPL** (spec §11.2). Prompt `shikra> `; input is read line by line from `rt_stdin` (the
same port user code reads); an entry continues until its parentheses balance outside strings,
`|symbols|`, `;` and `#| |#` comments and `#\(` characters. Each datum of the entry is read
(pure immortal), expanded with `front_repl_expand` (or `front_repl_import` for `(import ...)`),
compiled as a unit and run; the value is printed with `write` (nothing for the unspecified
value; each of several values on its own line; values start on a fresh line). With stdin not
a terminal the input lines are echoed (a piped session reads like the spec's transcript). An
error prints its message and irritants (compile errors: `error: msg`, located when the source
has positions), drops the rest of the entry, resets the dynamic state and returns to the
prompt; definitions survive (`front_repl_recover` forgets half-loaded libraries). End of input
prints a newline and exits 0. `,quit` exits.

*Environment model.* Without a file the session is `front_repl_new()` importing `(scheme base)
(scheme case-lambda) (scheme cxr) (scheme write) (scheme process-context) (scheme eval)
(scheme load) (scheme repl)` -- every built-in library there is today. With `shikra repl
main.scm` the program is loaded as for `build` (closed top level, program dir and `-I` for
libraries), run (errors print and the prompt still comes), and its top-level frame is reopened
as the session: its imports and definitions are available and redefinable. `import` at the
prompt adds bindings (later imports override in the open frame). `(interaction-environment)`
is the session; in `shikra interp` it is a fresh open top level with the default imports, made
on first use. `(environment set ...)` is a new open top level with those imports (definitions
by `eval` go into it). `eval` copies its datum (scanned immortal), expands it in the
environment's frame (an `(import ...)` datum imports), compiles a unit and tail-calls it; front-
end errors inside become Scheme errors ("malformed if", "library (x) not found") catchable by
`guard`. `load` is Scheme: read each datum with `%read` and `eval` it before reading the next.

*,expand expr*: expands in a scratch frame whose parent is the session (definitions do not
take effect) and prints each core form with `unparse` (renaming stripped). *,c name*: the
session records the source of its imports and definitions (and of the loaded file); `,c`
writes the imports (minus the REPL-only libraries) and the latest definition of each name to a
temporary program, runs `shikra emit-c -I <progdir> [-I dirs] FILE` (argv[0]) and prints the
functions of that definition: the block commented `/* main: name */` whose C name is
`P_main__<x>`, and every function named `P_main__<x>__...` (its nested procedures and
case-lambda clauses). Messages for a name the session did not define, a definition that is
not a procedure, and a session the compiler rejects (its diagnostic is shown above).

**How to add an instruction**: an `OP_` in interp.h (document its operands there), emit it in
bytecode.c (keep `depth()` accounting exact: maxstack sizes the frame), a `case` in `run()`
(vm.c) that calls `SYNC()` before anything that can allocate or raise, and a test in
tests/cases (it then runs under both engines).

**Tests.** `tests/run.sh interp`: the 41 tests/cases under `shikra interp` against the compiled
expectations (one `NAME.compiled-only`: reentry, whose compiled run pins the M5 tier-3 error;
the interpreter prints the R7RS answer, tested in tests/repl/callcc), and 13 REPL transcripts:
spec (the spec's session verbatim), multiline, errors, redefine, expand, c, quit, eof,
eof_nonl, load_file (+ a user library and `import` at the prompt), eval (eval, environment,
interaction-environment, load), callcc (generator, re-entered dynamic-wind, top-level re-entry,
multiple values to a continuation), deep (10^6-iteration tail loop, 10^5-deep recursion, apply
on 10^5 elements). r7rs: base.scm interpreted is 658/658 (`expected/base.interp`; its whole
output equals the compiled run's except the pinned tier-3 test); eval.scm interpreted is 1/3
(`expected/eval.interp`: the other two use `(scheme inexact)`, M5). tests/errors
repl_only_eval and repl_only_load. `make test`: 332 checks, 2 reported skips, 3:24.

**Numbers** (Apple M-series): start-up of `shikra repl`/`interp` (read, expand and compile
every built-in library, run hello world) about 3 ms; r7rs base 7 ms. `fib 30`: interpreter
122 ms, compiled (-O2) 4.5 ms: 27x.

**Engine disagreements found** (all fixed on the interpreter side): quoted data were mutable
(now read-only, see Memory); a literal `call-with-values` receiver given the wrong number of
values reported an arity error instead of the compiled wording (VCHECK). No compiled-engine
bug was found.

**Known gaps**
- The interpreter is not optimised: every library procedure call (`car`, `+`) is a full VM
  call through its global cell; no superinstructions.
- A continuation copies the whole stack (no segmenting); deep stacks make capture and re-entry
  proportionally slow.
- `eval`/`load` copy their data into immortal memory and front-end structures are never freed:
  a session that evaluates unboundedly many forms grows (templates are leaked too).
- Middle-end diagnostics of the compiler (arity of known calls, provably wrong types, values
  counts) are run-time errors in the interpreter.
- Circular data given to `eval` loops in the copy.
- `,c` text extraction relies on the emitter's comment and naming conventions; `define-record-type`
  or `define-values` names are not recognised by `,c` (only `define`).

### M5 libraries (all "Full" libraries of spec §3.1 except tier-3 continuations)

**File map** (runtime 3,225 → 3,724 lines in `rt.h` + `rt_*.c`, not counting the generated
`rt_unicode.c`: 393 lines, of which ~45 are code and 6.3 KB are table data; Scheme library +190)

```
src/rt_num.c      decimal conversion: small fixed big integers (bn_*), Burger-Dybvig printing,
                  exact-ratio parsing; (scheme inexact) primitives over rt_sys_* math
src/rt_io.c       datum labels (io_scan pre-pass, io_print with labels), write-shared/-simple,
                  binary file ports, file-exists?, delete-file, file errors; symbol bar rule
src/rt_core.c     address tables (rt_addrtab_get), equal? on cycles, promises, command line,
                  environment, clocks; the symbol table moved to OS memory (see bugs below)
src/rt_str.c      (scheme char): classes, simple and full case mappings, ASCII or Unicode
src/rt_unicode.c  GENERATED by tools/mkunicode.c (UCD 18.0.0), committed; --unicode=full only
src/rt_sys.c      rt_sys_{exp,log,sin,cos,tan,asin,acos,atan2,sqrt}, rt_sys_exists/remove/environ
src/expand.c      delay / delay-force lowered to %make-promise
lib/scheme/{char,file,inexact,lazy,read,time}.sld, process-context.sld and write.sld completed
tools/mkunicode.c generator (Makefile: make unicode UCD=dir)
```

**Promises** (`(scheme lazy)`). A promise is a heap object `RT_T_PROMISE` with one field, a
*state pair* `(done? . value)`, or `(#f . thunk)` while pending. `(delay-force e)` expands to
`(%make-promise #f (lambda () e))`, `(delay e)` to `(delay-force (%make-promise #t e))`
(expand.c, keywords exported by `(scheme lazy)`). `force` is Scheme, the R7RS reference
algorithm: call the thunk; unless the promise got done meanwhile (reentrant force), copy the
returned promise's state into this one's state pair and make the returned promise share that
pair; loop (a self tail call: a C `goto` compiled, constant VM stack interpreted). A chain of
10^7 `delay-force` steps runs in 2.6 MB RSS compiled under a 256 KB C stack, and in the
interpreter. `make-promise` returns a promise unchanged; `force` of a non-promise returns it.

**Datum labels** (`write`, `display`, `write-shared`, `write-simple`; rt_io.c). `rt_print(x,
port, mode)` with `RT_PRINT_WRITE` / `_SHARED` / `_SIMPLE`. A scalar, or SIMPLE, prints directly.
Otherwise a pre-pass walks the pairs and non-empty vectors depth first with an explicit stack
(a raw heap buffer of (object, next child) words, so long lists do not recurse) and records each
object in an address table with flags SEEN, OPEN (on the current path) and LABEL. An object met
again while OPEN (a back edge, i.e. a cycle) gets LABEL; with SHARED, any object met again does.
The printer then writes `#n=` at the first occurrence of a LABEL object (numbers from 0 in print
order, kept in the table) and `#n#` afterwards; a labelled object in a list's cdr position ends
the list with ` . #n#`. So `write` labels exactly the cycle targets ("labels only when there are
cycles"), shared acyclic structure prints in full, and `display` and the error printer (irritants
are printed with `write`) terminate on cycles. Records, ports, promises print opaquely and are
not traversed. *Address tables* (`rt_addrtab_get`, rt_core.c) are open-addressing tables in a raw
heap buffer, keyed by one or two object addresses; the buffer is reached from the C stack and
the keyed objects from the caller's arguments, and the collector does not move objects, so no
rooting is needed.

**equal?** First a plain recursive comparison (recursing on cars and vector elements, iterating
on cdrs and the last vector element) with a budget of 100,000 pairs/vectors and a depth limit of
1,000; if either runs out, a second comparison records every (a, b) pair of compound objects it
enters in an address table and treats a pair met again as equal. That is the bisimulation (the
greatest fixed point), which is R7RS's `equal?` on infinite trees, and it terminates because the
table only grows. Deeply car-nested acyclic data still recurses on the C stack (as before).

**Float printing and parsing** (rt_num.c, no libc). Big integers: `num_big`, 130 32-bit limbs
(4,160 bits), little-endian, normalised; operations set, multiply-add by a small number, times
10^k, bit length, shift left, shift right by one, compare, subtract, add -- each `RT_NOINLINE`
(inlined copies doubled the text size at -O2).
- *Printing*: Burger & Dybvig's free-format algorithm with exact r/s/m+/m- (the half gaps;
  unequal gaps at powers of two), the decimal exponent estimated as ceil((e2 - 1) log10 2) with a
  32-bit fixed-point constant (never too large; one fixup step), digit generation with the
  IEEE round-half-even acceptance of the interval ends, ties in the last digit rounding up.
  Output: positional for 1e-7 <= |x| < 1e21 (`0.0000001`, `123.456`, `100000000000000000000.0`),
  otherwise `d.ddd` + `e` + sign + exponent with at least one fraction digit (`1.0e+21`,
  `5.0e-324`, `1.7976931348623157e+308`). That exponent form is the one spelling that satisfies
  every `test-precision` of tests/r7rs/write.scm (which accepts `5.0e-324` but not `5e-324`, and
  `1.7976931348623157e+308` but not `...e308`) and the `test-numeric-syntax` spellings of
  read.scm; no test conflicts with it.
- *Parsing*: the significant digits (at most 780; further digits only set a sticky digit) and
  the exponent make an exact ratio num/den (decimal exponent applied to the side where it is
  positive; radix 2/8/16 and inexact ratios `#i1/3` likewise); early outs for values beyond
  1e310 (infinity) or below 1e-325 (zero). The quotient's 54 leading bits are computed by
  restoring division (55 compare/subtract/shift steps), the shift chosen from the bit lengths
  and clamped at 2^-1075 for subnormals; round to nearest even with the remainder as sticky
  bit; the double's bits are `((lsb exponent + 1074) << 52) + mantissa`, so a carry into the
  exponent and the subnormal/normal boundary need no special case. Exponent markers `e s f d l`
  (any case) are all accepted as `e` (R7RS 7.1.1).
- *Validation*: tests/unit/rt_unit.c (part of `make test`) compares against the host libc:
  100,000 random bit patterns must print text that `strtod` reads back to the same bits and that
  is no longer than the shortest `%.*e` that round-trips, plus 2 x 100,000 parses (`%.Ne` of
  random doubles, random 1-30 digit strings with exponents -360..340) against `strtod`, plus
  every power of two and its neighbours, plus the classic hard cases (2^-1074, 5e-324 and the
  halfway point 2.4703282292062327/8e-324, 2.2250738585072011e-308, DBL_MAX and the overflow
  boundary, 9007199254740993, 0.1+0.2, 1e23, 8.41e21, 7.2057594037927933e16, the exact halfway
  decimal of 1 + 2^-53 and its neighbours). A throw-away harness additionally checked 10^6
  random bit patterns (0 failures, 0 longer than the libc shortest, 2.25 million parses equal to
  `strtod`) and 180,000 exact halfway points between adjacent doubles (and the decimals just
  above and below them, up to ~1,700 digits) generated with Python's `decimal` (all equal to
  `strtod`). tests/cases/float_text.scm checks the same through `number->string`,
  `string->number` and the reader in both engines.

**Unicode tables** (`--unicode=full`; spec §3.1, §9.1, §11.1). tools/mkunicode.c reads
UnicodeData, SpecialCasing, CaseFolding, PropList and DerivedCoreProperties of UCD 18.0.0 (URLs in
its header) and writes src/rt_unicode.c: *run tables* -- varint sequences of (gap, count*2 +
stride-1[, zigzag delta]) over code points >= 0x80, stride 1 or 2 (stride-2 runs capture the
alternating upper/lower blocks), decoded linearly -- for Alphabetic, Uppercase, Lowercase,
White_Space and Nd (digit value = index in the run mod 10, checked by the generator), and the
simple upper, lower and fold (C+S) maps; plus the unconditional full mappings of SpecialCasing
(upper: 103 entries such as ß → SS, ǰ → J̌; lower: İ → i̇). Full case folding (status F) is
derived at run time as "simple-fold each character of the full upper case of the lower case"
(after the full lower case, for İ); the generator verifies that rule for every code point and
would emit the exceptions as a table (there are none in 18.0.0). Sizes: Alphabetic 1,643 bytes,
Uppercase 395, Lowercase 452, White_Space 19, Nd 186, upper map 796, lower 738, fold 827, full
upper 1,236, full lower 12 (6.3 KB). A throw-away check compared all 8 lookups for all 1,113,984
code points >= 0x80 with values computed independently from the UCD files: 0 mismatches (it
found a decoder off-by-one in stride-2 runs during development). A lookup decodes ~0.5 µs on
average. `string-downcase` turns Σ into ς when the previous character is cased and the next one
is not (R7RS allows either form; the tests accept both).

*Gating.* ASCII rules are in rt_str.c (`str_prop`, `str_map`): non-ASCII characters are caseless,
not alphabetic/numeric/whitespace without the tables. Program mode: the emitter appends
rt_unicode.c and defines `RT_UNICODE_FULL` only with `--unicode=full` (`RT_HAVE_UNICODE` guards
every reference), and cond-expand/`features` then see `full-unicode`. Tool mode: the tables are
always compiled in; `shikra repl` and `shikra interp` take `--unicode=full`, which sets
`rt_unicode_full` and the feature (default ascii, the same as compiled programs).
`char-ci*`/`string-ci*` compare `char-foldcase` / `string-foldcase` results (so `(string-ci=?
"STRASSE" "straße")` with full Unicode).

**Other libraries.** `(scheme inexact)`: one primitive per function over one platform function
each; `(atan y x)`; `(log z b)` = log z / log b; `sqrt` of an exact non-negative integer that is
a perfect square is exact (via `exact-integer-sqrt`), otherwise a flonum. `(scheme file)`: file
ports are OS-descriptor ports (textual or binary); open and delete failures raise errors of kind
FILE ("open-input-file: cannot open file" + the name); `call-with-*` use `call-with-port`;
`with-input-from-file`/`with-output-to-file` parameterize the current port and close the file
when the thunk returns. `(scheme read)`: `read` = `%read` on the current input port (the shared
reader; programs that do not call it link neither the reader nor the symbol table).
`(scheme process-context)`: `command-line` is argv (`shikra run FILE a b` / `shikra interp FILE a
b`: FILE, a, b); `get-environment-variables` is an alist in `environ` order. `(scheme time)`:
`current-second` is POSIX time as a flonum, jiffies are monotonic nanoseconds,
`jiffies-per-second` is 10^9. `write` puts bars around symbols that would not read back as
themselves, by a syntactic rule (a leading digit, a sign or `.` before a digit, `+i`/`-i`,
infnan prefixes) that no longer calls the number parser. The REPL's default environment imports
every built-in library (base, case-lambda, char, cxr, file, inexact, lazy, process-context, read,
time, write, eval, load, repl).

**What M6 must provide in the platform layer** (no libc): `rt_sys_pow`, `rt_sys_exp`,
`rt_sys_log`, `rt_sys_sin`, `rt_sys_cos`, `rt_sys_tan`, `rt_sys_asin`, `rt_sys_acos`,
`rt_sys_atan2` (also used for one-argument `atan` as atan2(x, 1)), `rt_sys_sqrt` (the hardware
instruction is enough: IEEE sqrt is correctly rounded); `rt_sys_exists` (stat or access),
`rt_sys_remove` (unlink), `rt_sys_environ` (envp from the initial stack), `rt_sys_clock_ns` for
both clocks (clock_gettime), plus the M2/M3 list (write, read, open, close, poll, exit, mmap,
munmap, the setjmp equivalent, mem*). Float printing and parsing need nothing from libc.

**Pay for use** (spec §9.1; Apple clang, macOS arm64). Hello world: 70,144 → 70,408 bytes
(`__text` 21,060 → 20,368; the file grows only by symbol-table entries of the new small
functions): the float printer got smaller and the parser is no longer linked (the symbol bar
check is syntactic); the datum-label pre-pass (generic `display`, uncaught-error printer) is new
(see Deviations). A `--unicode=full` build of tests/cases/char_unicode is 91,008 bytes against
74,144 for the ASCII build of the same body. `nm` of hello world and of a program using only
fixnums, strings, lists and `display`: no `rt_read`, `rt_intern`/symbol table, `io_open_file`,
`rt_open_*_file`, `rt_uni_*`, `rt_sys_exp`/math, `num_convert`/`num_ratio`; `rt_format_double`
and the `bn_*` helpers it needs are linked because generic `display` can print a flonum (M4).
`--unicode=full` adds the 6.3 KB of tables only to programs built with it.

**Tests.** `tests/run.sh r7rs` runs every tests/r7rs program in both engines with pinned
expected files (char-unicode with `--unicode=full`; eval in the interpreter); the stress pass
runs all of them too. New tests/cases: write_labels, read_datums (+ stdin), float_text, files,
process_time (`NAME.args`, `NAME.exit`), char_ascii / char_unicode (shared body
inc/chars.scm; `NAME.flags`), lazy_space (10^7-step chain, R7RS and SRFI 45 streams,
reentrancy; stress interval 1,000); tests/errors/malformed_delay. Driver: `NAME.flags` (extra
flags for build and interp), `NAME.args` (program arguments). rt_unit: the float corpus.

**Bugs found in existing code**: (1) the symbol table was a heap vector rooted by one global;
when it grew while the REPL front end allocated in unscanned ("pure") immortal mode, the new
vector was unscanned and mortal symbols in it were collected (the REPL crashed in `,c` once
more libraries were loaded): it now lives in OS memory as a dynamic root range. (2) The REPL's
default import form was built before switching to immortal mode and then recorded in the
unscanned `recorded` list (`,c` printed a mangled import). (3) `equal?` recursed on circular
data forever (known gap, fixed). (4) Writing symbols like `+i`, `-i`, `+NaN.0abc` without bars.

**Known gaps**
- Conservative stack scanning can keep a lazy stream alive: a stream head left in a stale slot
  or callee-saved register of a compiled caller frame retains the forced prefix (SRFI 45's
  "filter that skips 10^6 elements" retains ~175 MB compiled, 7 MB interpreted; when the
  stream is created inside the consuming procedure the compiled run is constant too). Stack
  space is constant in both engines. Spec §15 lists this risk of conservative scanning.
- Run-table lookups are linear (~0.5 µs per non-ASCII character).
- Datum labels cost a hash-table entry per pair/vector printed (write of compound data is a few
  times slower than M4 will need for large outputs; `write-simple` skips it).
- `char-ready?`, `read-line` etc. on file ports are unchanged from M1; there is no `O_EXCL`,
  append mode, or directory listing (not R7RS).
- Tier-3 continuations remain (the pinned failure in tests/r7rs/expected/base.compiled).

### M4-1 (the structural optimizer)

**File map** (compiler 4,770 → ~6,700 lines: `opt.c` 1,600 new; normalize.c +160, emit.c +100,
front end +80, ir.h +45; runtime +45; library +25)

```
src/opt.c         the passes over the IR: census, recursion SCCs, simplify (propagation,
                  folding, dead code, join points, inlining), contify, unbox, merging of
                  top-level mutual tail recursion, tier-1 verification, ir_finish (escapes,
                  tail-call graph, direct signatures, lambda lifting, closure layout)
src/normalize.c   optimistic tier-1 sites, first top-level forms (undefined checks), unused-
                  definition warnings, the rounds driver ir_build, shared constructors
src/emit.c        direct signatures, parallel jumps, RT_TAIL/RT_DTAIL/RT_NT, stack receivers
src/rt.h, rt_core.c  trampoline, tier-2 k walks the winders (rt_engine_travel), RT_ALWAYS
src/library.c, expand.c, front.h   unused-import tracking, cwarning, global.nowarn
tests/shape/      structural checks of the emitted C;  tests/warnings/  expected warnings
tests/fuzz.sh, tests/fuzz/gen.scm  randomized differential tester (not in make test)
```

**Pipeline** (`ir_build`): rewrite_aliases, tree shaking on core forms (roots unchanged: all
user code, impure built-in forms, raise), first top-level forms, unused-definition warnings;
then rounds of { normalize → `ir_optimize` → `ir_tier1_failures` }: a round whose optimistic
tier-1 sites failed rebuilds the IR from the core forms with those sites demoted to tier 2
(each round demotes at least one; in practice at most one extra round). Then `ir_finish`, C
names, def/use counts. **M4-2's analysis plugs in after `ir_optimize` (or as one more pass
inside its round loop) and before `ir_finish`**; `ir_finish` must stay last (it changes
signatures and removes closure creations).

`ir_optimize` runs rounds (3 at -O0, 6 at -Os, 8 at -O2) of
1. **census**: liveness by a worklist from the roots (main, `keep` procedures: raise, travel-to!,
   `--keep`), through creation nodes, known calls and references to known procedures; resets
   and recomputes `nrefs`, `nsets`, `ncalls` (references that are the callee of a known
   call), `njumps`, the `owner` of every binder and join point, the `parent` of procedures; a
   let-bound lambda in an unassigned temporary becomes a known procedure. Procedures are never
   unlinked during optimization (dead = not reached, or `dead` set); `ir_finish` filters.
2. **recursion**: Tarjan SCCs over known calls, references to known procedures and creation
   (a procedure "contains" the procedures it creates); `rec` = can reach itself. Copies
   inherit `rec`.
3. **global constants**: a top-level definition never `set!` whose value is a constant or a
   known top-level procedure: unchecked references read it (`subst`); the store disappears
   when nothing reads the global.
4. **simplify** (one walk per procedure, nested lambdas at their creation, so a binding is seen
   before its uses): substitution through `ir_var.subst` (never for checked reads); calls whose
   callee became a known procedure get `known` (only with a matching argument count: a wrong
   count stays the run-time arity error); folding of pure primitives of constants (fixnum
   `+ - *` without overflow, comparisons, `eq?`, `eqv?` except flonums, `car`/`cdr` of a
   constant pair, type predicates, `char->integer`) -- never an application that would raise;
   `if` on a constant; dead bindings (atoms, lambdas, primitives without PF_FAIL/PF_EFFECT,
   stores to unread globals); unused FIX members; join points without jumps; **a join point
   with one jump** (not an optimistic tier-1 continuation) moves to the jump, its parameters
   becoming lets; **inlining** (below).
5. **contify** (below), 6. **unbox**: a box whose every use is `%box-ref`/`%box-set!` in the
   procedure that binds it becomes an assigned C local (`V_ASSIGNED`).
Then once: **merging** of top-level mutual tail recursion (below), census.

Counts may be stale within a round but never too low: every constructor that adds a
reference or a jump increments (`mk_ref`, `mk_jump`), so "unused" and "one jump" are safe.

**Inlining.** A known call (plain procedure or case-lambda clause, `ctl` none) is inlined when
the callee is live, not `rec` (nor its dispatcher), not the procedure being simplified, and
either its size (nodes, nested procedures included) is at most `lim_small` (-O0 6, -Os 12, -O2
40) and the growth budget allows (-O2: total program size + 20,000 nodes; -Os: 1/4 + 2,000;
-O0: 1/8 + 500), or it is the only reference to the procedure (size up to 4,000; not at -O0).
The copy (`cp_*`: fresh variables, join points and procedures, procedures of one FIX in two
steps so they can call each other) is entered through a join point holding the parameters
(the callee's loop, if it has one); in value position the callee's returns jump to a join
point whose parameter is the call's variable and whose body is the rest of the caller, and its
tail calls become calls followed by that jump. Rest parameters are consed at the call. The
copy is simplified at once (so wrappers of wrappers dissolve in one round). This dissolves the
library: `(car x)` is `%car`, `(+ a b c)` nested `%+` (explicit 3/4-argument clauses), cxr
chains, `(display x)` the clause applying `%display` to `(current-output-port)`, record
procedures the record primitives, and `for-each`/`map`/`vector-for-each`/`assoc` with a lambda
become loops calling the lambda's inlined body. **-O0** inlines only procedures of at most 6
nodes (wrappers), does not inline the only call of large procedures, and runs 3 rounds;
everything else (contification, constants, direct signatures, tree shaking) runs at every level.

**Contification** (spec §8.1). Groups: the candidate members of one FIX, a let-bound procedure,
or a top-level procedure. Candidate: live plain procedure (no rest, not a clause, not kept)
whose every reference is a known call and every call site was found. The group is contified
into procedure Q when every call from inside the group is a tail call (in the member's own
body, not a nested procedure) and every call from outside is in Q, Q not inside the group, and
either all those calls are tail calls (the members' entries become join points of Q, placed
where the group was bound if that is in Q, else at Q's entry) or there is exactly one (then
the join points go to that site; for a non-tail site the members' returns and tail calls
continue at a new join point holding the rest of Q). Member bodies move (they are not copied);
internal calls become jumps; the binding disappears. Sites moved by an earlier contification in
the same pass are retried next round (`s->in->dead`, slot validity checks). This turns named
let, do, local `define` loops and state machines (`even?`/`odd?`, lexers) into labels of one C
function, and helpers called from one place into its code.

**Merging of top-level mutual tail recursion.** Top-level known procedures that never escape
(every reference a known call) and form a cycle of tail calls among themselves (Tarjan on that
graph) become one procedure `<first>-group (which a...)`: its body is the members' entries as
join points, preceded by a dispatch on `which`; tail calls inside the group jump; every other
call passes the member's selector and pads the arguments with `#f`. What still uses `musttail`:
tail calls to unknown procedures, to escaping or case-lambda procedures, and cycles that pass
through them (`apply`, continuation-passing closures, procedures stored in data).

**Calling conventions** (the rule that keeps tail calls proper). After optimization `ir_finish`
computes `escapes` (the closure may reach unknown code: kept, or a reference that is not the
callee of a known call, or a case-lambda dispatcher that does) and the **tail-call graph**:
known tail calls are edges to the callee; unknown tail calls (and `%apply`/`%call-with-values`
in tail position) are edges to a node *unknown* with edges to every escaping procedure;
dispatchers have edges to their clauses (`rt_call_cc` calls from C: no edge). A procedure is
**direct** -- C signature `obj f(params...)`, no self, no argc, no arity check -- when it does not
escape, is not a case-lambda dispatcher or clause, has at most 64 parameters and is on **no cycle
of that graph**. Calls into and out of a direct procedure are plain C calls (`RT_DTAIL`): since
no cycle passes through it, a chain of tail calls traverses those plain calls a bounded number
of times, so the C stack stays bounded. Everything else keeps the uniform signature and
`RT_TAIL` (`musttail`). fib, tak, ack and local non-tail recursive procedures are direct;
tests/cases/tail_calls.scm, control_tail.scm and lazy_space.scm (10^6-10^7 iterations; the
last under a 256 KB stack) check that tail calls stay proper, also under the trampoline.
Direct procedures drop unused parameters (unless they loop), take a rest list built by the
caller, and get their free variables as extra parameters (**lambda lifting**: free variables to
a fixpoint through calls of other direct procedures, renamed innermost first; callers pass
them, nested closures capture them as needed); their closure creations disappear. Uniform
procedures check `argc` only when they escape. A known call to a static non-escaping uniform
procedure passes `#f` as `self` (`ir_call_self`).

**Tier 1, widened** (spec §8.2). A `(call/cc (lambda (k) body))` site of base's `call/cc` where
`k` is only called (never a value, never `set!`) is *exact* (as before) when no call of `k` is
inside a lambda, *optimistic* otherwise: it is normalized as tier 1 with a join point for `k`
(`ir_join.site`), and calls of `k` inside lambdas become jumps to that join point from another
procedure. After optimization `ir_tier1_failures` requires every jump to such a join point to
be in the C function that declares it and no `%set-winders!`/`%set-handlers!` in the region
(inlined dynamic-wind, parameterize, with-exception-handler would be skipped by the jump);
failing sites are rebuilt as tier 2 (the whole IR is rebuilt from the core forms). So a `k`
called from a lambda given to an inlined `for-each`, or from a contified loop, costs nothing.
Any other jump to a join point from another procedure is an internal error.

**Tier 2, cheaper.** A compiled continuation now walks the winders itself (core_cont_code calls
the generated `rt_engine_travel`, the library's `travel-to!`, when the winders differ, then
re-checks that it is still live), so every non-tier-1 call of base's `call/cc` in compiled code is
`%call/cc` directly: no escape procedure, no rest list, no `apply`. A capture is the
`rt_call_cc` frame on the C stack (jump buffer, saved lists) plus a 3-word `k`; a literal
receiver used only by that `%call/cc` is a closure on the C stack (`stack_closure` in emit.c).
ctak 2.76 s → 1.35 s, fibc 1.62 → 0.77 (baseline → M4-1). The interpreter keeps the library's
escape procedure. The dead-continuation error message is unchanged.

**Undefined checks and constants.** `first_forms` walks the top-level forms in order: a known
procedure first becomes reachable at the first form that references it (transitively through
procedures); its reads of globals whose definition comes earlier are unchecked. Together with
global constants this removes `rt_chkdef` from nearly all global reads (record type
descriptors, top-level vectors).

**Tree shaking and warnings** (spec §9, §11.3). User code is normalized whole (diagnostics), then
the census keeps only what the roots reach; procedures inlined everywhere or contified vanish.
Warnings (`file:line:col: warning: ...` on stderr, never failing a command, silenced by `-w`):
`unused definition NAME` for a definition of the program or of a user library not reachable from
expressions, impure right-hand sides and user-library exports (record procedures and hygienically
renamed names are exempt: `global.nowarn`); `unused import SPEC` for an import set of the program
or a user library none of whose bindings was ever looked up (expand.c's `lookup_in` notes uses).
The REPL's `,c NAME` runs `emit-c -w --keep NAME`: the definition is a root, kept as its own
(uniform) function and shown, even if every call to it is inlined.

**Trampoline** (spec §8.1). Without `__attribute__((musttail))`, or with `SHIKRA_NO_MUSTTAIL`
(hidden `--no-musttail`), `RT_TAIL(f, ...)` stores the callee and its arguments in globals and
returns `RT_TRAMP` (0x707, not a Scheme value); every non-tail call is `RT_NT(call)`, which calls
pending callees until a real value returns; `rt_call_cc`, `rt_program_main` and
`rt_engine_raise` use it too. Arguments beyond a3 stay in `rt_args` meanwhile. Direct
procedures may return `RT_TRAMP` from their tail calls; their callers' `RT_NT` runs it.

**IR changes.** `IR_LETJ` declares a chain of join points (`ir_join.next`, mutually recursive);
`ir_join.site` (optimistic tier-1), `state`; `ir_var.ncalls`, `subst`, `mark`; `ir_lambda.dead`,
`keep`, `escapes`, `direct`, scratch fields; `ir_program.travel`, `opt`; `ir_build` takes
`ir_opts` (level, warnings, `--keep` names). Invariants kept by every rewrite: unique binders,
ANF, join points jumped to only from their own procedure (after `ir_tier1_failures`),
`call.known` is a live procedure whose arity matches, `ir_var.type` copied with its variable.

**What M4-2 can rely on.** After `ir_optimize`: wrappers are gone (primitives appear directly,
with their prims.def type letters and PF flags), loops are join points (their parameters are
the loop variables), every call site of a non-escaping procedure is known and listed by
`collect_sites`, `ir_var.known`/`call.known` are exact, `type` is 0 everywhere and is copied by
inlining. Census gives owners and counts. Direct signatures and lifting happen later, so the
analysis sees closures and their free variables as written; private conventions (unboxed
arguments, results in registers) extend `direct` in `ir_finish`/emit.c.

**Debugging aids** (environment of the `shikra` tool): `SHIKRA_TRACE=1` prints every inlining,
contification and merge; `SHIKRA_CHECK=1` verifies after each pass that every term chain ends
in a tail; `SHIKRA_ROUNDS=n` limits rounds; `SHIKRA_NOINLINE`, `SHIKRA_NOCONTIFY`,
`SHIKRA_NOUNBOX`, `SHIKRA_NODIRECT` switch passes off; `SHIKRA_CMAX=n` stops after n
contifications (bisecting).

**Tests.** tests/run.sh: compiled cases and the compiled r7rs runs at -O2, -O0 (64 MB stack) and
-Os; `tramp` (every case and compiled r7rs file at -O0 with `--no-musttail`); `shape` (8 programs,
directives on the emitted C: spec §12 `sum-to` loop in one function with no call and no closure,
`for-each` with an assigning lambda (no closure, no box, no unknown call), record accessor,
nested named lets, local and top-level mutual recursion, tier-1 escape from a `for-each`, cxr /
variadic arithmetic / comparisons / `vector-fill!`); tests/warnings (expected stderr, and `-w`);
tests/cases/optimizer.scm; `gcc` mode (not in `all`). tests/fuzz.sh N SEED: random programs
(top-level procedures over integers with named lets, closures with `set!`, `for-each`/`map`/
`apply` with lambdas, call/cc escapes from loops and lambdas, letrec state machines,
case-lambda, vectors, printing inside expressions, non-tail local recursion) compared between
the interpreter and -O0, -O2, -Os and the trampoline.

**Numbers** (Apple M5, macOS 27.3, Apple clang; r7rs-benchmarks as in bench/README.md).
- Benchmarks, safe, 53 runnable, all `ok` at -O2 and with `--unsafe` (CSV `m4-1-20261119-0955`,
  `m4-1-unsafe-20261119-0957`; RESULTS.md). Geometric mean of Shikra/Chez **0.91** (baseline
  1.36; the M5 tree without the optimizer, `m4-0` on a quiet machine, 1.05), Shikra/Gambit
  **0.83** (baseline 1.24, m4-0 0.96); unsafe/Chez-unsafe 0.98 (baseline 1.47). Against m4-0,
  M4-1 is 0.87x in geometric mean: graphs 0.28x, conform 0.47x, peval 0.53x, matrix 0.56x,
  lattice 0.56x, earley 0.57x, ctak/fibc ~0.66x; slower on tiny recursive benchmarks where clang
  generates different code for the same work (tak 1.33x, fibfp 1.22x, cpstak 1.22x, takl 1.18x;
  timings on this machine vary by ±15%: fib is 1.46-1.84 s depending on whether clang inlines
  the slow path, rt.h `RT_ALWAYS`). equal is 2.4x slower than the baseline already in m4-0 (the M5
  `equal?` with cycle detection), unchanged by M4-1.
- Binary sizes (bytes, -O2 / -Os): hello world 69,920 / 70,416 (before: 70,400 / 70,880); fib
  70,224 / 70,576 (70,832 / 71,504); nucleic, the largest benchmark, 494,144 / 482,448
  (539,480 at -O2 before).
- Compile time of tests/r7rs/base.scm (6,000 lines with its libraries): `emit-c` 0.08 s → 0.12 s;
  `build -O2` 1.7 s → 2.1 s (cc dominates); `build -O0` 0.67 s.

**Known gaps / weaker than specified**
- Tier-1 verification failures rebuild the whole IR (one extra round, rarely two).
- Contification needs all outside calls in tail position or a single call site; a procedure
  called non-tail from two sites with the same continuation (both branches of an `if` in value
  position) stays a procedure (direct, so a plain C call).
- No jump threading through a join point whose body tests its parameter (`(if (not x) ...)`
  materialises a boolean; the C compiler threads it). FIX members referenced only by each other
  but unreachable are kept until nothing references them at all.
- (M4-2: the port parameters are static cells; analysis-known calls.) Parameter objects (`current-output-port`) are closures stored in globals: `(display x)` still
  makes an unknown call to read the port (M4-2: the analysis knows the global's value).
- (Resolved in M4-2.) Hello world still links the collector (the library's `make-parameter` allocates) and generic
  `display` (M4-2/M4-3, type-directed specialisation and pay-for-use).
- Mutual recursion through procedures that escape (stored, passed as values) still uses
  `musttail`; so do cycles through case-lambda clauses.
- `vector-map` on one vector collects into a list before building the vector (re-entry safety);
  `string-for-each`/`string-map` still go through `string->list` (UTF-8 indexing is O(n)).
- Optimistic tier-1 sites are rebuilt if the region holds an inlined winders/handlers change even
  when the jump does not cross it (conservative).

**Deviations recorded**: none new. Two entries of §10 are resolved (tree shaking spares user
code; no trampoline). The front end now notes every identifier lookup in user libraries for
the unused-import warning (`front_note_use`), which the REPL ignores.

### M4-2 (the flow analysis and what it enables)

**File map** (compiler, counted as in M4-1 without the interpreter: 6,237 → 7,931 lines; `analyze.c`
1,265 new -- the analysis ~900, representation selection and values structs ~350; emit.c +206,
opt.c +167, normalize.c +14, ir.h +41; runtime 3,750 → 3,969 (+220:
(rt.h fixnum/flonum helpers, unchecked accessors and writers, `RT_SAFE`; rt_io.c port cells,
fd-only writes, flat printing; rt_core.c the handler-less error path, `equal?`); library: port
parameters and `parameterize`)

```
src/analyze.c   ir_analyze: lattice, abstract interpretation with path facts, rewrites, diagnostics
                ir_represent: unboxed fixnums/flonums, private signatures, values structs
src/opt.c       jump threading, %values folding, provenance of inlined code (N_LIB, V_FAR),
                one inlining budget across runs
src/emit.c      representations and conversions, specialised primitives, private signatures,
                values structs, RT_NO_HANDLERS / RT_NO_FLONUMS
tests/shape/    sum_to_main, float_loop, values, hello (+ nm-no / nm-has / count directives)
tests/errors/type_*, tests/warnings/types_*, tests/cases/types_*, tests/fuzz/gen2.scm
```

**Pass order** (`ir_build`): rewrite_aliases, tree shaking, first forms, unused-definition warnings;
then rounds of { normalize → `ir_optimize` → `ir_tier1_failures` → up to three times { `ir_analyze`
→ if it rewrote structurally: `ir_optimize` (the remaining inlining budget) → `ir_tier1_failures` } }
(a failing tier-1 site rebuilds the round as before); then the final `ir_analyze` (types,
diagnostics; its rewrites are not followed by the optimizer), `ir_finish`, `ir_represent`, C
names, counts. The analysis runs only on IR whose tier-1 sites are verified (every jump in the
function of its join point). `SHIKRA_NOANALYZE=1` skips it (all types unknown), `SHIKRA_NOREPS=1`
keeps every variable an obj; `SHIKRA_TRACE=1` prints iterations and term walks.

**Lattice.** An abstract value is `{t, f, r, lt}`: `t` a bit set (ir.h `T_*`): fixnum, flonum,
char, `#t`, `#f`, pair, null, string, symbol, vector, bytevector, procedure, record, eof,
unspecified, port, the three standard ports (stdin, stdout, stderr: own bits), the three static
port-parameter cells (pairs with own bits), record-type descriptor, the letrec undefined marker,
other (boxes, promises, error objects, values objects); `f` the one procedure (lambda) it can be,
or "many"; `r` the one record type (the `%make-rtd` site, when that site runs at most once: in the
program body outside loops) or "many"; `lt` the types it can have judging by its procedure's own
code alone (diagnostics). Join is union; two different procedures join to "many" and both escape;
meet (refinement) intersects. Monovariant: one value per variable, per procedure result, per box
(boxed variable), per record field of a record type, per port cell.

**Constraint generation per IR node** (one walk per reached procedure body per round):
`CONST` its type; `REF` the variable's value met with the innermost fact about it; `LAMBDA`/`FIX`
`{procedure, that lambda}`; `SET` joins into the variable (globals: closed world, all assignments);
`PRIM` the result type from prims.def's letter refined per primitive (+ - * on fixnums give
fixnums -- overflow raises --, any flonum gives flonums; predicates decided by the argument type;
`eq?`/`eqv?`/`equal?` of disjoint types #f; `%make-record` the record type of its rtd; record
fields; box contents; port cells; `%uncaught`/`%exit` nothing); a checked argument that can never
have the right type makes the result empty; `CALL` known (M4-1's `known`, or the one lambda the
operator can be): arguments into the callee's parameters, the callee's result (the callee is walked
first in this round if it was not yet: a chain of calls costs no extra rounds); unknown: every
argument escapes, the result is any; `%apply`/`%call-with-values` with unknown receivers: escape,
any; `%call/cc`: the receiver gets a continuation (unknown procedure), the result is any; `JUMP`
arguments into the join parameters, and the jump's facts into the join's facts; `RET` and tail
calls into the procedure's result. A procedure is reached from the program body, `keep`
procedures (raise, travel-to!, `--keep`: they also escape), known calls and escapes.

**Where "any" enters**: parameters of escaping procedures (closures that reached unknown code,
were stored in data, passed to unknown calls or joined with another procedure), results of unknown
calls, `car`/`cdr`/`vector-ref`/`%values-ref`... of data, record fields of an unknown record type,
values read from ports, `string->number` (number or #f).

**Refinement.** Facts are a scoped list (at most 64): after a checked primitive returns, each
checked argument variable has the checked type (`car x` ⇒ x is a pair); in the branches of `if t`
with `t` a type predicate of x: x ∩ P / x \ P (exact predicates); `record?`: x is that record type;
`eq?`/`eqv?` with a singleton-type constant both ways; `if x`: x \ #f / x = #f; comparisons
`< > <= >=` record order relations both ways (x < y and y > x; the negation in the else branch:
only used for fixnums, so NaN does not matter); results of `length`, `*-length`, `char->integer`,
`bytevector-u8-ref` are in [0, 2^48). A join point that is not a loop starts with the facts that
hold at every jump reaching it (so `(when (not (pair? x)) (error ...)) (car x)` is proven: the
error branch never reaches the join, `error` never returns); a loop join starts with the facts at
its declaration. Only variables never assigned are refined. Jump threading in the optimizer
(a join whose body only tests its parameter, entered with a constant, goes to the branch) keeps
`not`, `and` and `or` from hiding the predicate.

**Rewrites** (final walk of each analysis): a pure primitive whose result is decided (and whose
checked arguments are proven) becomes the constant; an `if` on a decided test becomes its branch;
the rest of a term after a call or primitive whose result is empty becomes `return` (it never runs);
a call whose operator is exactly one procedure with a matching clause becomes a known call (no
`RT_CALLABLE`, no argc check, direct entry). Then the optimizer runs again (newly known calls are
inlined, dead join points and procedures vanish). Annotations: refined types on atoms (`ty`), on
primitives `S_OK(i)` (argument i proven) and `S_NOOVF`.

**Overflow-check elimination** (`S_NOOVF`): `(+ x c)` / `(- x c)` with constant c and proven
fixnums is unchecked when a dominating relation bounds x: x < y or x <= y with y a constant K and
K (- 1) + c within the fixnum range; x < y (x > y) with y a fixnum variable and c = 1 (c = -1);
one step of transitivity through a constant bound on y (lengths are below 2^48). The section 12
loop `(if (> i n) acc (loop (+ i 1) ...))` has i <= 1000000 in the else branch, so `i + 1` is a
plain add of tagged words and only `acc + i` is checked.

**Specialisation** (emit.c `special`): both fixnums: `+ - *` on tagged intptr_t words (`rt_fxadd`
etc.: `__builtin_*_overflow`, the overflow calls `rt_fx_overflow`, which raises "+: fixnum overflow"
with both irritants, as the generic path), compare directly, `quotient`/`remainder`/`modulo`
inline (generic call for the raising cases); a flonum and a proven number: double arithmetic
(`rt_num_dbl` for a number of unknown kind), comparisons when both are flonums (or one is a fixnum
constant within ±2^53), `sqrt exp log sin cos tan asin acos atan inexact abs negate` straight to
the platform functions; `car cdr set-car! set-cdr! vector-ref vector-set! string-ref
bytevector-u8-ref *-length char->integer record-ref record-set!` without the type check when proven
(bounds and literal checks stay; a record access is one load); boxes are never checked (the
compiler's own); `eqv?` with a non-flonum side and `equal?` with an immediate/symbol/procedure/
record side are pointer comparisons; `display`/`write`/`write-char`/`write-string`/`newline`:
string, char and fixnum writes (`rt_put_*`, no checks) when the port is proven to be the open
standard output or error port, the checked string/char writers otherwise, `rt_display_flat` /
`rt_write_flat` (no datum labels) for values proven not to be pairs or non-empty vectors, the
generic printer only for the rest.

**Representations and boxing points** (`ir_represent`, after `ir_finish`): a variable whose type is
exactly fixnum is an `intptr_t` holding the tagged word (free conversions); exactly flonum a
`double`, except that it stays an obj when it would be boxed twice or boxed after being unboxed: a
boxing use is a non-float primitive argument, an unknown or uniform call's argument, a jump or
`let` into an obj variable, a return from a procedure with an obj result, a capture by a closure,
an `if` test; a boxed source is an obj variable, a uniform procedure's parameter, a non-float
primitive or call result (fixpoint, demotion only). Globals, boxed variables, closure slots and
`self` are always obj. Flonum literals are static data and C double literals.

**Private calling conventions.** A direct procedure (M4-1 rule: no escape, on no tail-call cycle,
not a case-lambda) gets parameters in their representations (`intptr_t`/`double`), unused
parameters dropped, free variables passed as parameters, no closure argument, and its result in
the representation of its result type (fixnum, flonum: if not boxed twice, or a values struct).
Tail calls stay proper: a direct procedure is on no cycle of the tail-call graph, so its outgoing
calls are plain C calls a bounded number of times in a row; one that returns an unboxed value never
returns the trampoline marker, so its tail calls to uniform procedures are plain calls followed by
the conversion, and calls to it are not wrapped in `RT_NT`.

**Values struct** (spec §8.5). A direct procedure all of whose returns are `(values a b ...)` of
the same count n (n >= 2) or tail calls of such procedures, and every one of whose non-tail calls
binds a variable used only by `%values-check` / `%values-ref` (a known receiver: `let-values`,
`call-with-values` with a literal lambda), returns `rt_mvN` (`struct { obj v[N]; }`) by value: no
values object. When the producer is inlined instead, `%values-check`/`%values-ref` of a variable
bound to `(%values ...)` fold in the optimizer.

**Pay for what you use** (spec §9.1, as implemented):

| Row | Criterion |
|---|---|
| Collector | linked iff something reachable references `rt_alloc_slow` (allocation fast paths), and `--gc=bump` not set. Analysis-driven: proven writes to the standard ports go through `rt_port_write_fd` (no string-port branch, which allocates), and with no handler the error path allocates nothing |
| Exception machinery | `RT_NO_HANDLERS` when no reachable `%set-handlers!` installs a handler (its argument is not the current list or its cdr): runtime errors print and exit directly (`core_die`, no error object, no Scheme `raise`); linked at all only if some reachable check remains |
| Symbol table | linker: `rt_intern` is referenced only by `string->symbol` and `read` |
| Float printing | `RT_NO_FLONUMS` unless a flonum literal (also inside quoted data) or a primitive that makes a flonum from non-flonums (`/ expt inexact exp log sin cos tan asin acos atan sqrt current-second string->number read`) is reachable; then the printer is linked only if a printer is (`rt_format_double` from the generic printer, `number->string`, the flat writers) |
| Float parsing | linker: `string->number`, `read` |
| Datum reader | linker: `read` |
| File ports | linker: `(scheme file)` procedures |
| Cycle detection (datum labels) | linker: `io_scan` from the generic `display`/`write`, linked when one is reachable on a value whose type may be a pair or a non-empty vector, or by the uncaught-error printer (irritants of any type), not by fixnum overflow reports |
| Stack-copying continuations | `RT_TIER3` when some `%call/cc` site is tier 3 (escape analysis, "Tier 3 continuations as built"): `rt_call_cc3`, the re-entry code, the conservative scan of stack copies, `rt_program_main` kept out of `main` |
| Unicode tables | `--unicode=full` |

The port parameters: `current-input-port` etc. are closure-free case-lambdas over static cells
`rt_port_cells[k]` = `(converter . value)` (rt_io.c, a root range): `(current-output-port)` is
`RT_CDR(rt_port_cell(1))`, a load. `parameterize` binds them shallowly: their winders frame is a
dynamic-wind frame whose before/after thunks swap the value in and out of the cell, and
`parameterize-run` enters and leaves frames with `travel-to!` (before: `%set-winders!` directly),
so escapes and re-entry swap correctly. The REPL resets the cells with the dynamic state.

**--unsafe** (spec §3.3): `RT_UNSAFE` removes every `RT_CHECK` in the runtime (type checks), the
bounds checks of the inline accessors (`RT_INDEX`), the record-type check, the literal checks
(pairs, vectors), `rt_chkdef` (undefined variables), `RT_CALLABLE` (not a procedure), the
fixnum overflow checks (`rt_add` & co. and `rt_fxadd` wrap, through unsigned arithmetic), the
division-by-zero fallbacks of the inline integer division; the emitter drops the argc check of
escaping procedures. The analysis' refinements after checked primitives are trusted as in safe mode.
Not removed: checks inside out-of-line runtime functions that are not `RT_CHECK` (port state,
ranges of `substring`, `string->symbol` of a non-string through `io_check`...).

**Diagnostics policy** (spec §11.3). A primitive argument, call operator or record access whose
type can never be right: a compile error when the proof is local -- the value's type follows from
the procedure's own code (`lt`: constants, primitive results, its tests; standard library and
record procedures inlined into it count as primitives, a user procedure's parameter or result does
not, even when inlined: `V_FAR`, `N_FAR`) -- and the code runs whenever the body does: no undecided
`if` on the way, not after a handler was installed in the same body (an inlined guard body), and
the body is the program body, a top-level procedure, or a local procedure only ever called
directly (not one passed to other code, which may run it under a handler). Otherwise a warning
("... (an error when it runs)", once per location, silenced by `-w`), and the code still raises.
Library code guarded by its own test is never reported (deliberate error paths); dead code is
never reported. Arity mismatches of calls the analysis made known are warnings. Inlined standard
library code takes the location of the user's call.

**Known imprecision** (examples): values read from input (`(read)`, benchmarks' `hide`) are any,
so `fib` of the r7rs suite stays generic (it is called with a number read from stdin); a loop
variable that starts with an unknown value and then gets flonums (`sumfp`: `i` starts as `(hide
count input1)`) stays an obj and is boxed per iteration; contents of pairs and vectors are any
(`(vector-ref v i)` of a vector of flonums is generic: spec §12); procedure values joined at one
site lose both (`(if c f g)` called later is an unknown call; flat lattice, no sets); one record
type per `%make-rtd` site that runs once (types defined in a procedure are "many"); values from
`%values-ref` of a struct are any (no per-position types); fields of a record type are joined over
all records of that type; no interval analysis beyond dominating comparisons and lengths (`(- n 1)`
after `(= n 0)` keeps its overflow check); facts are not kept for assigned variables; loop joins
get only the facts of their declaration.

**Numbers** (Apple M5, macOS 27.3, Apple clang; machine shared with other work).
- Benchmarks (CSV `m4-2-20261119-1220`, `m4-2-unsafe-20261119-1222`; RESULTS.md): 53/53 ok in both
  modes. Geometric mean Shikra/Chez **0.78** (m4-1 0.91), Shikra/Gambit **0.71** (0.83);
  unsafe/Chez-unsafe **0.74** (0.98), unsafe/Gambit-unsafe 0.79 (51 common); unsafe/safe 0.88
  (m4-1: 1.0, nothing was removed). Against m4-1 in that run: 0.86x in geometric mean. Single runs
  vary by 10-20% on this machine (an earlier full run gave 0.71x Chez), so the main benchmarks were
  also compared interleaved, best of three, m4-1 binary vs m4-2: mbrot 6.20 → 0.27 s (0.04x:
  doubles in registers, no boxing), equal 0.87 → 0.30 (0.35x, the in-place switch to the cycle
  table; the pre-M5 baseline was 0.44), sumfp 4.22 → 2.76 (0.65x), ray 0.83x, pnpoly 0.85x,
  simplex 0.87x, fibfp 0.88x, nucleic 0.95x, nqueens 0.95x, fib 0.96x, fft, tak, ack, sum, primes
  1.0x. The fixnum loops gain nothing: the suite passes every input through `(read)` and `hide`,
  so `fib`, `tak`, `ack`, `sum` see "any" parameters (the same programs with literal arguments
  compile to the intptr_t loops of the shape tests: `fib` above, spec §12). Worst in the run: cat
  1.17, divrec 1.11, diviter 1.08 (I/O and list code the analysis does not change: noise).
- Sizes (bytes, -O2 / -Os): hello world 50,888 / 50,904 (M4-1: 69,928 / 70,424; `__text` 1,568);
  fib with a literal argument 51,144 / 51,160 (70,216 / 70,552); the section 12 program 51,104 /
  51,136. No collector, no `raise`, no generic printer in any of them (shape tests `hello`,
  `sum_to_main` check `nm`).
- Compile time (`emit-c`, mean of 10): tests/r7rs/base.scm 104 ms (M4-1) → 117 ms (three analyses
  of 15,600 variables and 2,250 procedures, 4 rounds each, ~19,000 term walks, plus the second
  optimizer run); nucleic 16.9 → 19.9 ms. `build -O2` of base.scm 1.5 s, `-O0` 0.54 s (cc dominates).
- Tests: `make test` 831 checks (2 skips as before); `tests/run.sh gcc` with gcc-16; tests/fuzz.sh
  about 3,000 programs (gen and gen2 for 1,500 seeds at -O0, -O2, -Os and the trampoline): the first
  1,000 found two bugs, both fixed (an analysis-known call to a closure was inlined where its free
  variables were out of scope; a `set!` of a local whose binding had been deleted survived), the
  2,000 after the fixes found none.

**What M4-3 needs**: `shikra size` can attribute the specialised runtime pieces (`rt_put_*`,
`rt_fx*`, flat printers) and the two config macros; `-g` locations can reuse the inlined-code
locations (N_LIB copies carry the user's call site); the expansion cache is unaffected (the analysis
runs on IR).

### Tier 3 continuations as built (M5)

**File map** (runtime +111 lines → ~4,080 in `rt.h` + `rt_*.c` without the Unicode tables:
`rt_core.c` +87, `rt_gc.c` +13, `rt.h` +11; compiler +430 → ~8,380: the classifier in `analyze.c`
~390, normalize.c/opt.c/emit.c/ir.h the rest; library: the escape procedure)

```
src/analyze.c   ir_classify_callcc: reachability and procedure uses, free variables, the taint
                analysis (tterm/trhs/tprim/tcall), escapes, CAP (procedures a capture may resume)
src/normalize.c ir_forcebox (names of assigned variables that must be boxed), the round loop
src/opt.c       unbox leaves forced boxes alone
src/emit.c      rt_call_cc3 for N_TIER3 sites, #define RT_TIER3
src/rt_core.c   core_cont_code's dead path (CORE_DEAD), rt_call_cc3, core_callcc3_run,
                core_reenter, core_rewind; rt_continuation_live_p is #f for compiled continuations
src/rt_gc.c     RT_T_CONT objects scanned conservatively (flagged mark-stack ranges)
src/rt.h        rt_call_cc3; rt_program_main never inlined into main under RT_TIER3
lib/scheme/base/control.scm   the escape procedure travels only where the engine does not
tests/cases/{gen_fringe,coroutines,amb,reentry*,tier3_escape}.scm, tests/shape/callcc_tier{2,3},
tests/fuzz/gen3.scm
```

**Classification** (spec §8.2). Tier 1 is unchanged (M1 part A, M4-1: optimistic sites verified
after optimization). Every remaining `%call/cc` site is classified after the final flow analysis
(normalize.c's round loop: `ir_analyze(prog, 1)` then `ir_classify_callcc`), on the IR that is
emitted. A site is **tier 2** when its continuation `k` provably cannot be invoked once the
site's `rt_call_cc` frame is gone from the stack; otherwise **tier 3** (`N_TIER3` on the call
node, counted in `p->tier3`).

The analysis is a taint propagation over the whole program, 64 sites per pass (a bit per site;
more sites take more passes). A value is *tainted by site s* if it may be, or contain, a
continuation of s. Seeds: the receiver's parameter. Flows: `let`, join-point parameters, `set!`
of locals, known calls (arguments into parameters, a rest list from the extra arguments, results
back to the call), returns and tail calls into the procedure's result; a closure is tainted by
its free variables (and by the contents of the boxes it captures); allocating primitives give
their result the union of their arguments' taints and accessors (`car`, `vector-ref`,
`record-ref`, `%values-ref`...) the taint of their argument -- immutable data built from `k` is
followed, not given up; a box set by the procedure that binds it records its contents; the
winders and handler lists are two abstract locations: `%set-winders!`/`%set-handlers!` add to
them and `%winders`/`%handlers`/`%parameter-binding` read them (so `dynamic-wind` before/after
thunks, `parameterize` values and `with-exception-handler` handlers that capture `k` do not make
it escape), and `travel-to!` gets the winders' taint as its parameter (the runtime passes saved
winders). An atom whose analysed type cannot hold a reference (fixnum, flonum, char, boolean,
string, symbol, bytevector...) carries no taint. A call `(k v ...)` whose operator is the
parameter of a receiver used only as a receiver delivers `v ...` as those sites' call/cc values.

Site s **escapes** (tier 3) if a value tainted by s may reach: an argument of an unknown call,
of `apply`, or of a continuation other than the receiver's own `k`; a mutating primitive's
stored argument (`set-car!`, `vector-set!`, `record-set!`, `%promise-set-state!`, a box of
another procedure...), `%uncaught`/`%exit`; a `set!` of a global; the result of a procedure that
unknown code may call (a procedure whose value is used other than as the callee of a known call
or the receiver of a call/cc: stored, passed, returned, kept); the value of s's own call/cc (the
receiver returns it, or `k` is passed to itself); and s escapes when its receiver is not one
known procedure. What survives is `k` passed down to known procedures -- inlined or analysed
library procedures (`for-each`, `map`, `vector-for-each`, `dynamic-wind`,
`with-exception-handler`, `parameterize`), user recursion, helper procedures -- that only call
it or pass it further down.

*Soundness argument.* Invariant: at run time a reference to a continuation `k` of an activation
of s exists only (1) in C frames of activations inside that activation's extent, (2) in
immutable data and closures reachable only from those, (3) in the winders and handler lists,
whose entries are pushed and popped in LIFO order by activations inside the extent and are
restored together with the stack by every continuation, and (4) in tier-3 stack copies taken
inside the extent. Values move between activations only by arguments, returns, closures,
mutation of shared data or globals, and continuation invocation; the analysis follows the first
three into known code and treats every way into unknown code, every mutation that can make the
value reachable from elsewhere, and the return out of the extent as an escape. Monovariance
(all activations of s share one bit) is safe because a continuation of an earlier activation can
only reach a later one through an escape. Case (4): a tier-3 copy taken inside the extent brings
the site's frame back when it is copied back, and the re-entry revives it (below), so `k` is live
again exactly when the stack that contains it is current. Unknown calls are covered by giving up
on every value passed to unknown code and on the results of every procedure unknown code may
call; the runtime calls into Scheme only through receivers (followed), `travel-to!` (winders
taint) and `raise` (runtime errors; see the deviation on irritants). The dead-continuation check
of tier 2 (`free[0] == #f`) stays as a cheap guard: if the analysis were ever wrong, the call
raises "tier-2 continuation called after its extent (bug)" instead of jumping into a dead frame.

*Imprecision* (tier 3 although no re-entry happens): `k` stored anywhere, even a local box of an
enclosing procedure (`(set! return k)`); a value tainted only on some paths is tainted on all
(per variable); a `guard` whose clause calls an outer continuation (`(call/cc (lambda (return)
(guard (e (#t (return #f))) ...)))`): the clause's thunk reaches guard-run's call/cc value, which
guard-run may return, so `return`'s site is tier 3 (guard's own site stays tier 2; restructuring
guard-run to call a returned thunk makes it precise but costs a closure per normal guard exit,
~20% on a guard microbenchmark, so it was not done); procedures passed to procedures that call
them through a parameter count as escaping (at -O0, where little is inlined, `map` with a lambda
that returns `k`-tainted data).

**Assigned variables.** A copied frame brings back the values its C locals had at capture time;
for a Scheme variable that is assigned (a location, R7RS 3.4) re-entry must see the current
value, as the interpreter does (it boxes every assigned variable). So when some site is tier 3,
`CAP` marks the procedures that may be on the stack, to be resumed, during a tier-3 capture: the
procedure holding the site, callers through known calls, procedures with an unknown call (or
apply / call-with-values) when some escaping procedure is in CAP, procedures holding a call/cc
whose receiver is in CAP (fixpoint). Every assigned, unboxed local owned by a CAP procedure gets
its name added to `ir_forcebox` and the round is rebuilt from the core forms (like a tier-1
demotion): the normalizer boxes assigned variables of those names, the optimizer's unbox pass
leaves them boxed. Names are unique per binder (the expander renames locals apart), so inlined
copies are boxed too. Join-point parameters (loop variables) are bindings, not locations:
restoring them is correct.

**Runtime representation.** A tier-3 capture is the tier-2 `core_cont` frame (jump buffer, saved
winders and handlers, receiver, `k`, value) plus: `k` has 3 free slots -- `free[0]` the frame
while it is live on the current stack (else `#f`), `free[1]` the frame address always, `free[2]`
the stack copy: a heap object of header type `RT_T_CONT` (raw class) `{hdr, lo, words...}` holding
the bytes from `lo` (an address in `core_callcc3_run`'s frame, which lies below `rt_sys_catch`'s)
up to the stack base `rt_init` recorded. Tier-2 `k`s keep 1 slot; `core_cont_code` serves both.

**Capture** (`rt_call_cc3`): push the frame on `core_conts`, `rt_sys_catch` (setjmp: the
registers go into the frame's jump buffer) calls `core_callcc3_run` (never inlined: GCC would clone
`rt_sys_catch` and inline it, putting the copy's start above the jump-buffer frame), which checks
that the stack grows down from below the base, allocates the copy, copies, stores it in
`free[2]` and calls the receiver. Cost: one allocation and one copy of the live stack.

**Invocation of a live k** (tier 2 or 3): as before -- `travel-to!` the capture's winders if they
differ, mark the frames above dead, set the value, restore the lists, throw.

**Re-entry** (`core_reenter`, a dead tier-3 `k`): (1) pack the value(s); (2) compute the common
tail of the current and the captured winders (the captured list is read from the frame's image
inside the copy) and `travel-to!` it: after thunks run on the current stack, innermost first, in
the dynamic state outside each frame (R7RS 6.10); (3) make room: a C99 variable-length array moves
the stack pointer 4 KB below `lo` if the current stack is shallower (re-entry from a shallower
stack; from a deeper one nothing is needed); (4) `core_rewind` (never inlined, never seen as
noreturn, so the call stays a call inside the array's frame): mark every frame of the current
`core_conts` dead, copy the stack back, make the captured frame the head of `core_conts` and mark
every frame of its chain live again (tier-2 frames inside the copy revive: their `k`s work
exactly while their frames are back on the stack), store the value, restore the handler list
(the winders list stays the common tail), throw to the frame's jump buffer; (5) the capture's
`rt_sys_catch` returns 1 on the restored stack and `rt_call_cc3` `travel-to!`s the captured
winders: before thunks run outermost first on the restored stack, where every frame they may
escape to is live; then it returns the value. Multi-shot: the copy is never modified. An escape
during an after or before thunk abandons the re-entry (consistent winders at every step). In
compiled code the library's escape procedure (first-class `call/cc`) only applies `k`, so the
before thunks never run on the wrong stack. `exit` in re-entered code unwinds the restored
winders. Runtime globals describing in-flight state: arguments beyond a3 are copied out of
`rt_args` by `core_cont_code` before anything runs; no trampoline call is pending at a capture or a
throw; `core_conts`, `rt_winders`, `rt_handlers` are restored as above; nothing else describes
the stack.

**Collector.** `gc_scan` pushes the words of an `RT_T_CONT` copy as a range whose `lo` has its low
bit set; `gc_drain` marks such ranges with `gc_mark_any` (conservatively, interior and untagged
pointers, as the stack), sliced like vectors, so chains of continuations never recurse. The jump
buffer inside the copy covers the saved registers. Stack addresses in the copy point outside the
heap and are ignored. A copy keeps alive what its frames referenced, including the `k`s of the
tier-2 frames inside it. All under `RT_TIER3`, so other programs' collectors are byte-identical.

**Pay for use** (spec §9.1). `RT_TIER3` is emitted when `p->tier3 > 0`; everything above is under
it. A program with only tier-1/2 sites compiles exactly the code it compiled before (only the
dead-continuation message string changed, and it is shorter): hello world, ctak, fibc, fib,
exceptions, dynamic_wind, control_tail, higher_order and the tier-1 shape program are
byte-identical in size and symbol list at -O2 and -Os (hello world 50,880 bytes, `__text`
1,568). callcc.scm (it returns `k` from a receiver: `(procedure? (call/cc (lambda (k) k)))`)
grows 72,784 → 89,440 bytes at -O2 (one more 16 KB text page), 72,944 → 73,136 at -Os;
tests/r7rs/base.scm 588,272 → 588,512.

**Portability** (what the Linux port must check). Plain C99 plus the existing jump-buffer wrappers;
no assembly. Assumptions, each checked or stated: the stack grows down (checked at every capture,
with the capture frame below the base: else a catchable error); the base given to `rt_init` lies
above every frame that can be live in Scheme code (main passes the address of its `argc`, and
under `RT_TIER3` `rt_program_main` is never inlined into `main`; an own `_start` must pass an
address above every frame it creates); memory between `lo` and the base stays mapped (it was
stack when captured); `longjmp` to a frame above the current stack pointer (glibc's
`__longjmp_chk` under `_FORTIFY_SOURCE` accepts that; mangled pointers in `jmp_buf` are restored
bit for bit); no shadow stack (deviation). The `__builtin_setjmp` path (GCC) and the `sigsetjmp`
path (Apple clang) both pass every test; `--cc gcc-16` on this machine exercises the former.

**Tests.** tests/cases: reentry (the R7RS 6.10 example, both engines; no `compiled-only` marker is
left in the suite), gen_fringe (tree-walking generators, same-fringe, a generator in a vector,
2·10^4 values), coroutines (ping-pong with 10^5 switches, round-robin threads), amb (multiple
dwelling, Pythagorean triples, eight queens: 92), reentry_wind (re-entered nested bodies three
times with the before/after trace, re-entry from inside another wind, escapes through winds, a
producer resumed in its wind, a before thunk escaping during the rewind), reentry_deep (captured
10^4 frames deep and re-entered 1,000 times from the top; from a 9,000-frame-deeper stack; inside
a 5,000-element map), reentry_values (0, 1, 2, 5 values, let-values, apply), reentry_dynamic
(parameterize, a port parameter, with-exception-handler, guard re-entered and catching again, an
error inside re-entered code), reentry_stored (list, vector, record, closure; assigned locals
shared by every re-entry; `(k k)`), tier3_escape (stored continuations used only as escapes,
10^5 times), reentry_map (map, vector-map, string-map, a user map: earlier results unchanged,
both engines agree), reentry_exit (exit in re-entered code runs the restored after thunks).
All pass under both engines, at -O0/-O2/-Os, with the trampoline, under GCC (-O2, -O0, -O0
trampoline) and under stress (collection at every allocation; compiled at -O2 and -O0, and the
interpreter; coroutines and tier3_escape use interval 10 in the interpreter's stress run, as
control_tail and exceptions do, for time). Shape: `callcc_tier2` (escape-only patterns at every
level: no `rt_call_cc3`, no stack-copying symbol linked), `callcc_tier3` (every re-entrant
pattern's site is `rt_call_cc3`), `hello` (no continuation code). tests/fuzz/gen3.scm: random
programs around call/cc (escapes through helpers, loops, map, traced dynamic-winds, handlers,
guard; stored continuations used as escapes; bounded re-entries, also into map and
dynamic-wind; assigned locals), compared between the interpreter and four compiled modes.
`SHIKRA_TRACE=1` prints each site's tier, `SHIKRA_TRACE=2` also the first reason each escapes.

**Numbers** (Apple M5, macOS 27.3, Apple clang; machine shared with other work, load ~9 during the
timings, so they are compared interleaved).
- Gate: tests/r7rs/base.scm compiled `658 tests, 658 passed, 0 failed` at -O2, -O0, -Os, -O0 with
  the trampoline, under stress at -O2 and -O0 (collection at every allocation), and with
  `--cc gcc-16` at -O2, -O0 and -O0 with the trampoline. Its only tier-3 site is the 6.10
  re-entry example; the other 14 sites of the file and the test framework's guard stay tier 2.
- ctak and fibc (r7rs-benchmarks): every site tier 2 (ctak 5, fibc 2), binaries identical in size
  and symbols; ctak 1.73 s vs 1.74 s, fibc 1.03 s vs 1.03 s (M4-2 binary vs this one, best of three,
  interleaved; 1.56 s / 0.89 s on the quiet machine before the change).
- Generators: 10^6 values from a re-entrant generator (two captures and two re-entries per value)
  take 0.26 s at -O2 (0.60 s at -O0; the interpreter 0.68 s) and peak at 2.6 MB RSS, the same as
  10^7 values (2.8 s): each capture copies a ~300-byte stack, old copies are garbage.
- reentry_deep: 1,000 re-entries of a 10^4-frame stack, 0.08 s.
- Compile time: `emit-c` of tests/r7rs/base.scm 176 → 214 ms (machine loaded; the classification
  plus one rebuild round for the boxing of assigned variables, since the file has a tier-3 site);
  programs without tier-3 sites pay only the classification.
- tests/fuzz.sh: 300 gen3 programs (seeds 41-340) and 300 gen/gen2 programs (seeds 5000-5149):
  no failure.
- `make test` (each mode of `tests/run.sh all` plus check-embed): 960 checks, 0 failures, no skip
  left (the interpreter runs reentry.scm too); `tests/run.sh gcc` (gcc-16): 192 checks, 0 failures.
