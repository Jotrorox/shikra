# Shikra 0.1 — compliance with the specification

Audit of the merged tree against `docs/shikra-spec-0.1.tex`, statement by statement, on macOS 27.3
arm64 with Apple clang 21 (20 November 2026). Every row was checked by running the tool or reading
the code, not from earlier reports. "Evidence" names a test of the suite (`tests/run.sh` mode or
file) that pins the statement, or the command used; experiments were run in a scratch directory.
Linux rows could not be re-run on this machine (no cross linker; Linux runs were out of scope) and
rest on DESIGN.md's recorded results; they are marked *(records)*.

## Summary

**201 statements checked (table rows below): 176 conform, 22 deviate, 3 not implemented.** All three "not
implemented" items are things the spec itself defers or names as fallbacks (§5.3 direct backend,
§7.2 later additions, §14 CPS fallback for tier 3, item 10); every required feature is present. Every deviation below is
recorded with its reason in DESIGN.md §10 ("Deviations from the spec").

Deviations, most significant first:

1. **Redefining an imported binding is accepted** (§4.1, §3.3; R7RS 5.6.1 says it is an error):
   `(import (scheme base)) (define (square x) ...)` shadows the import for the program's own code.
   Library code keeps the built-ins. Kept because ordinary R7RS code does this; a compile-time error
   rejected three suite programs. `set!` on an import is the §11.3 compile error.
2. **1,000–2,000-line programs exceed the size targets at -O2** (§9.3, §13 M4 "size targets are
   met"): macOS scheme 241 KB and dynamic 311 KB at -O2 (target 100–200 KB; at -Os 176 / 181 KB, which
   meets it); Linux -O2 186–312 KB against 60–150 KB. Hello world meets both targets.
3. **Line budgets exceeded** (§1.1, §5.2): runtime 4,886 lines (6,356 with extensions, own libm
   and bignums) against about 4,000; the flow analysis is 1,754 lines against "under about 1,000".
   The compiler and the interpreter are inside their budgets (table below).
4. **The receiver of a tier-2/3 `call/cc` is not in tail position** (R7RS 3.5): a loop that
   recurses through `call/cc` grows the C stack.
5. **`guard` clause tests see the raiser's `parameterize` bindings** (R7RS 4.2.7), a consequence of
   §8.3's "clauses on top of the raising frame". Matching clause bodies run in the guard's environment.
6. **A Linux program that uses `define-c` links the C library** (§9.2 "no libc"); all other Linux
   programs are static with no libc *(records)*.
7. **A top-level procedure can be called before its `define` is reached** without the error of
   §3.3; other globals are checked. Both engines behave the same.
8. **A provably wrong type is a compile error only when the proof is local** (§11.3); otherwise it
   is a warning and a run-time error.
9. **Stress mode collects at every allocation for 79 of 86 cases** (§13 M2): seven use a per-case
   interval because a full collection per allocation would be quadratic.
10. **Tier 3 does not work under hardware shadow stacks**, and the CPS fallback of §14 is not
    implemented.
11. **The conservative stack scan also accepts interior and untagged pointers** (§7 says "tagged
    pointer to the start"). This is needed for correctness under `cc -O2`.
12. **Generated C uses more than two compiler builtins** (§5.4), all behind feature tests with
    portable fallbacks. The static Linux runtime contains inline assembly (system calls, `_start`).
13. **Committed generated sources** (§5.4 "no generated sources"): `src/embed.c` and `src/rt_unicode.c`.
    The build itself has no generation step.
14. **`--target` accepts four targets** (§11.1): x86-64 and arm64 on Linux and macOS.
15. **Parameter objects are closures**, not fixed-field heap objects (§6.1).
16. **Two internal libraries are importable** (§10 names five): `(shikra primitives)` and `(shikra dynamic)`.
17. REPL details (§11.2): input is echoed when stdin is not a terminal; an uncaught error at the
    prompt does not run pending `dynamic-wind` after thunks; redefining an import at the prompt
    shadows it for session code only; interpreter stack overflow (2^25 slots) cannot be caught.
18. Library details without complex numbers or rationals (§3): `(sqrt -4)` is `+nan.0`;
    `current-second` is POSIX time; `expt` with a negative exact exponent follows `/`; the ratio
    syntax is limited to the fixnum range; `string-downcase`'s final sigma is simplified.
19. A literal `call-with-values` receiver given a provably wrong number of values is a compile
    error (stricter than §11.3's list).
20. graphs' peak RSS is 6.4× Chez's (non-moving collector). No spec statement sets a memory
    target; this is recorded against M4-3's own goal and has no row below.
21. The tier-2 escape analysis ignores continuations that escape as error irritants; calling one
    later raises a safe-mode error.
22. The compiler frees a few scratch buffers (§5.4 "frees nothing until exit"); IR and front-end
    data use the arena.

Additions (not deviations; listed in DESIGN.md §10): `--bignum`, `-o`, `-w`, `--no-cache`,
`--link`, `-l`, `--libc`, `size --all/--tsv`, `SHIKRA_CC`, `SHIKRA_CACHE_DIR`, hidden testing commands
and flags. Interpreter `define-c` is limited to functions of the tool's own process (at most 6
integer and 8 double arguments).

Fixed during the audit (each with a test; see git log): tests for every §9.1 row, `--gc=bump`
(§7.1, previously pinned by no test), `case-lambda`/cxr resolution (§3.1), feature identifiers
incl. `--target` (§3.4), flag-value validation (§11.1), library search order and shadowing (§4.3);
`--version` said "0.1 (M4)"; `repl --unsafe` was silently ignored (now a notice, §5.5); a bare
`,expand` printed nothing. DESIGN.md §10 was completed (items 1, 3, 12–16, 22 and the additions were
missing; items 7 and 9 were stale).

### Line budgets (§1.1, §5.2, §5.3, §7)

`wc -l` (physical lines; non-blank lines are 2–3% fewer). Lines average 40–55 characters; about 8%
are over 100 characters.

| Part | Files | Lines | Budget |
|---|---|---:|---:|
| Compiler (front, middle and back end) | library.c expand.c normalize.c analyze.c opt.c emit.c front.h ir.h | 8,388 | |
| + driver, expansion cache, `shikra size`, utilities | main.c cache.c size.c util.c util.h embed.h | 1,906 | |
| **Compiler and tool, total** | | **10,294** | about 15,000 |
| **REPL interpreter** | bytecode.c vm.c repl.c interp.h ffi.c (REPL `define-c`) | **1,616** | about 2,000 |
| **Runtime proper** | rt.h rt_core.c rt_gc.c rt_io.c rt_num.c rt_str.c rt_read.c rt_sys.c prims.def | **4,886** | about 4,000 |
| + extensions, own libm, bignums | rt_ext.c (518) rt_math.c (454) rt_big.c (498) | 1,470 | |
| + generated Unicode tables | rt_unicode.c | 393 | |
| Runtime, all | | 6,749 | |
| Flow analysis (§5.2) | analyze.c | 1,754 | under about 1,000 |
| C emitter (§5.3) | emit.c | 1,120 | about 1,500 |
| Collector (§7) | rt_gc.c (immix and bump) | 540 | about 1,200 |
| Not counted | embed.c (generated, 8,767), tools/mkembed.c, tools/mkunicode.c (360), lib/ Scheme (1,823) | | |

### Clean-checkout check

`git clone` of the repository into a scratch directory, then the README one-liner
`cc -std=c99 -O2 -Wall -Wextra src/*.c -o shikra` (no output, no warnings), `make check-embed`
(ok), `make clean && make` (regenerates `src/embed.c` identically; the clone stays clean), and
`tests/run.sh reader unit amalg expand driver` there: all passed. The one-liner with `-pedantic`
also gives no warning, and so does the generated C of the sample programs
(`cc -std=c99 -Wall -Wextra -pedantic`).

## §1 Overview

| Statement | Status | Evidence | Note |
|---|---|---|---|
| AOT compiler: whole R7RS program → one native executable | conforms | every `compiled` case | |
| Compiled programs contain no interpreter and no `eval` | conforms | errors/repl_only_eval, repl_only_load; `src/vm.c` is not in the embedded runtime | |
| A separate interpreter inside the tool provides a REPL | conforms | tests/repl | |
| G1 loops over fixnums, flonums, vectors compile to the hand-written C loop | conforms | shape sum_to_main, nested_loops, float_loop, ext_fx_loop, ext_fl_loop | |
| G2 hello world under 30 KB static on Linux | conforms *(records)* | shape mode Linux pin; DESIGN "Linux integration" 3.0–7.7 KB | not re-run here |
| G3 R7RS-small as written | conforms | r7rs mode, both engines; tests/r7rs/EXCLUSIONS.md | only spec restrictions excluded |
| G4 one binary, six commands, no dependency beyond a C compiler | conforms | `otool -L shikra`: libSystem only; `shikra --help` | hidden test commands exist |
| G4 budgets 15,000 / 2,000 / 4,000 | deviates | table above | runtime over (item 3) |
| Non-goals: no `eval`/`load` compiled, no R6RS/large/`syntax-case`, no threads, no full tower by default | conforms | `shikra check` of each `(scheme eval/load/repl)` import: error | `--bignum` is opt-in |
| Decision: C99 for compiler, interpreter, runtime | conforms | one-liner with `-pedantic`: 0 warnings | |
| Decision: whole program, closed world | conforms | §4 rows | |
| Decision: C99 backend compiled by `cc` | conforms | `shikra emit-c`; driver "plain cc out.c" | |
| Decision: bytecode REPL never in output binaries | conforms | nm of programs: no VM symbols | |
| Decision: 64-bit tagged words, 63-bit fixnums | conforms | rt.h tags | |
| Decision: mark-region, conservative stack, precise heap, bump allocation, no shadow stack | conforms | rt_gc.c; stress mode | interior pointers (item 11) |
| Decision: loops for known calls, `musttail` for the rest | conforms | shape mutual, sum_to; RT_TAIL in rt.h | item 4 |
| Decision: escape-only `call/cc` free, re-entrant copies the stack | conforms | shape escape, callcc_tier2, callcc_tier3 | |
| Decision: fixnum and flonum; overflow raises, never wraps | conforms | debug/locations, r7rs base 922/923 | wraps only under `--unsafe`, which removes checks (§3.3) |
| Decision: checked by default, `--unsafe` opt-out | conforms | cases ext_unsafe, big_unsafe; shape ext_*_unsafe | |
| Decision: UTF-8 strings with an ASCII fast path | conforms | rt_str.c `str_offset`; cases strings_utf8 | |
| Decision: `syntax-rules` only | conforms | expand.c; tests/expand | |

## §2 Name

| Statement | Status | Evidence | Note |
|---|---|---|---|
| `.scm` for programs, `.sld` for libraries | conforms | library.c; cases/libraries | |

## §3 Language

| Statement | Status | Evidence | Note |
|---|---|---|---|
| All of `(scheme base)` + ten optional libraries compiled; three evaluator libraries REPL only; complex, r5rs omitted | conforms | every R7RS export of the 14 libraries imported with `(only ...)`: all present; r5rs/complex: "not found" | |
| `(scheme base)` Full | conforms | r7rs base 658/658 | |
| `(scheme case-lambda)` Full, resolved at compile time for known calls | conforms | r7rs case-lambda; shape case_lambda (new) | |
| `(scheme char)` Full, ASCII tables unless `--unicode=full` | conforms | r7rs char, char-unicode; shape pay_ascii, pay_unicode (new) | |
| `(scheme cxr)` Full, inlined to chained loads | conforms | shape wrappers, case_lambda | |
| `(scheme file)` Full | conforms | r7rs file; cases files | |
| `(scheme inexact)` Full | conforms | r7rs inexact | NaN for complex results (item 18) |
| `(scheme lazy)` Full, `delay-force` in constant space | conforms | r7rs lazy; cases lazy_space (10^7 steps) | |
| `(scheme process-context)` Full | conforms | r7rs process-context; cases exit_* | |
| `(scheme read)` Full, linked only when `read` is reachable | conforms | r7rs read; shape pay_read, pay_symbols (new) | |
| `(scheme time)` Full | conforms | r7rs time | POSIX time (item 18) |
| Exact R7RS results where complex numbers or rationals would be needed (`sqrt` of a negative real, TAI `current-second`, ...) | deviates | DESIGN §10 "M5 libraries" | item 18 |
| `(scheme write)` Full, datum labels for cycles | conforms | r7rs write; cases write_labels | |
| `(scheme eval)` / `(scheme load)` / `(scheme repl)` REPL only | conforms | repl/eval; r7rs eval (interp); errors/repl_only_* | |
| `(scheme r5rs)`, `(scheme complex)` omitted | conforms | `shikra check`: "library ... not found" | |
| §3.2 exact integers are 63-bit fixnums; overflow raises | conforms | debug/locations; cases error_names; r7rs base | |
| §3.2 optional bignums later, behind a flag | conforms | `--bignum`; r7rs bignum, `*.bignum` runs | |
| §3.2 `(/ 1 3)` → 0.333… flonum, `(/ 6 3)` → exact 2 | conforms | r7rs base 916/917 rewritten; run: `0.3333333333333333`, `2`, `(exact? (/ 6 3))` #t | |
| §3.2 inexact reals are IEEE doubles | conforms | rt.h `rt_flonum` | |
| §3.2 `string-ref` constant on ASCII-only strings, linear otherwise | conforms | rt_str.c `rt_string_ref` | |
| §3.2 `syntax-rules` only, hygienic; no user code at compile time | conforms | expand.c; tests/expand | |
| §3.2 literal constants read-only, in read-only data | conforms | cases literals; `nm -m`: `rt_lits`, `rt_lit_pairs` in `__DATA_CONST,__const` | interpreter detects too |
| §3.3 safe mode raises a catchable error naming the procedure: types, ranges, argument counts, overflow | conforms | cases error_names; run: `(caught "vector-ref: index out of range" (5))` etc. | items 1, 7 |
| §3.3 every "it is an error" situation detected: a top-level procedure called before its `define` is reached | deviates | run: `(display (f 1)) (define (f x) ...)` prints 2 in both engines | item 7 |
| §3.3 `--unsafe` removes the remaining checks | conforms | shape ext_assume_unsafe, ext_dot_unsafe; rt.h `RT_UNSAFE` | |
| §3.4 `r7rs`, `shikra`, `ieee-float`, OS, architecture; `full-unicode` with the tables; never `ratios`/`exact-complex` | conforms | cases features, features_unicode; driver `--target` features (new) | also `posix unix little-endian lp64`, `bignum` with `--bignum` |

## §4 Compilation model

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Program and imported libraries compiled as one closed unit | conforms | emit.c; `emit-c` output is one file | |
| Known calls: never-`set!` top-level definitions are constants, direct C calls, arity checked at compile time, small ones inlined | conforms | errors/arity_known, arity_named_let; shape sum_to, wrappers | |
| Primitives stay primitive: imported `car`/`+` always the built-ins | deviates | run: `(define (car x) 42)` accepted | item 1 |
| Private calling conventions: unboxed integers/doubles, unused parameters dropped, no closure argument | conforms | shape values (`P_main__divmod(intptr_t, intptr_t)`), float_loop | |
| Tree shaking: unreachable code, library and runtime never emitted | conforms | shape hello (`nm-no P_scheme_base__`), warnings/unused | |
| Closed record types: field access is one tag check and one load | conforms | shape record | |
| Two engines share front end and runtime; one suite runs under both | conforms | interp mode (1 documented skip: ext_ffi_user) | |
| Every build recompiles everything; expanded libraries cached on disk | conforms | cache mode | |
| `(import (geometry vec))` → `geometry/vec.sld` in program dir, then each `-I`, then built-ins | conforms | driver: search order and shadowing (new) | a user library may shadow a built-in one |
| No package manager, no project file | conforms | | |

## §5 Compiler pipeline

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Eight passes over one IR, one C file to `cc` | conforms | main.c `gen_c`, `cmd_build` | |
| 1 Read: datums carry file, line, column | conforms | reader mode; positions in tests/errors | side table |
| 2 Resolve libraries: `define-library`, import sets, `include`, `cond-expand`, ordered graph | conforms | cases libraries; errors/library_cycle, lib_not_found | |
| 3 Expand: hygienic macros, derived forms lowered to `lambda if set! quote letrec*` + calls | conforms | expand mode | core also has `begin`, `define`, `%prim` forms (DESIGN §6) |
| 4 Normalize: unique names, `letrec*` grouping, boxes for captured+assigned, → IR | conforms | normalize.c; debug-ir | |
| 5 Analyze: procedures per call site, types per variable | conforms | analyze.c | |
| 6 Optimize: inline, fold, dead code, loops, closure removal, check deletion, unboxing | conforms | shape mode (69 programs) | |
| 7 Emit C: static data, one function per surviving procedure | conforms | emitted C | |
| 8 `cc` on generated file + runtime as one translation unit | conforms | driver "plain cc out.c" | |
| Front 1–3, middle 4–6, back 7; REPL branches after the front end | conforms | bytecode.c consumes core forms | |
| §5.1 IR is A-normal form with join points | conforms | ir.h | |
| §5.2 lattice: fixnum … procedure, each record type, any | conforms | ir.h `T_*`; analyze.c records per `%make-rtd` site | |
| §5.2 monovariant | conforms | analyze.c | |
| §5.2 analysis under about 1,000 lines | deviates | analyze.c 1,754 | item 3 |
| §5.3 C emitter about 1,500 lines | conforms | emit.c 1,120 | |
| §5.3 direct arm64/x86-64 backend later | not implemented | | "later" in the spec |
| §5.4 compiler, interpreter, runtime plain C99, no dependencies | conforms | `-std=c99 -pedantic` 0 warnings | |
| §5.4 generated code C99 plus two builtins behind macros, both with fallbacks | deviates | grep of rt.h/rt_*.c | item 12 |
| §5.4 build: one command, no configure step | conforms | clean-clone check | |
| §5.4 no generated sources | deviates | embed.c, rt_unicode.c committed | item 13 |
| §5.4 no existing Scheme needed | conforms | | |
| §5.4 compiler allocates from arenas, frees nothing | deviates | util.c arena; some `free` | item 22 |
| §5.4 IR nodes one tagged union, a `switch` per pass | conforms | ir.h | |
| §5.4 reader and value representation written once, shared by compiler, REPL, programs | conforms | rt_read.c, rt.h in tool and programs | |
| §5.4 standard library mostly Scheme, embedded as text | conforms | lib/ (1,823 lines) in embed.c | |
| §5.5 interpreter shares passes 1–3, compiles core forms to bytecode, never the optimizer/C backend | conforms | bytecode.c, vm.c | |
| §5.5 same tagged values, collector, primitives | conforms | interp mode output identical | |
| §5.5 explicit value stack; tail calls constant space; continuations copy that stack | conforms | repl/deep, repl/callcc; interp of a 10^7-deep tail loop | |
| §5.5 open top level, redefinition | conforms | repl/redefine | |
| §5.5 always checked, no unsafe mode | conforms | repl/unsafe (new) | |
| §5.5 never shipped | conforms | embed list has only rt*, lib/ | |

## §6 Value representation

| Statement | Status | Evidence | Note |
|---|---|---|---|
| `...0` fixnum `n << 1` | conforms | rt.h `RT_FIX` | |
| `001` pair, two words, no header | conforms | rt.h `rt_pair`, `rt_cons` | |
| `011` procedure: code address + captured values | conforms | rt.h `rt_proc` | header sits before the code word |
| `101` other heap object: header + fields | conforms | rt.h `RT_HDR` | |
| `111` immediates: char, `#t`, `#f`, `()`, eof, unspecified | conforms | rt.h | plus internal markers |
| Fixnum add/sub/compare on tagged words; multiply untags one operand | conforms | rt.h `rt_fxadd`/`rt_fxmul`; shape sum_to_main | |
| `pair?`/`procedure?` without memory access; `car` = tag check + load | conforms | rt.h `rt_is_pair`, `rt_car` | |
| Pairs take 16 bytes | conforms | pair-only blocks of 16-byte cells (rt_gc.c) | |
| §6.1 8-byte header: 8 bits type, 8 flags, 48 length | conforms | rt.h | |
| Vector, bytevector, string (chars, bytes, buffer; ASCII flag), symbol (static, by address), flonum, record (descriptor + fields) | conforms | rt.h structs; emitted literals | |
| Port, promise, error object: fixed fields | conforms | rt.h | |
| Parameter: fixed fields | deviates | parameters are closures | item 15 |
| Closure-free procedure is a static constant | conforms | emitted `static const rt_proc C_...` in `__DATA_CONST` | |
| §6.2 generic flonum: 16-byte box; unboxed locals, loop variables, known arguments/results; static literals | conforms | shape float_loop, ext_fl_loop; emitted literals | |
| §6.2 floats in generic vectors are boxed; `(shikra f64vector)` covers it | conforms | cases ext_f64vector | |

## §7 Memory management

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Non-moving Immix-style mark-region, conservative stack, precise heap; no shadow stack, stack maps or write barrier | conforms | rt_gc.c | |
| 32 KB blocks of lines; bump through free lines; inlined add, compare, store | conforms | rt_gc.c (256-byte lines); rt.h `rt_alloc`, `rt_cons` | |
| Mark from roots, free unmarked lines; side-table mark bits | conforms | rt_gc.c | |
| Stack word that is a tagged pointer to a live object's start keeps it alive | deviates | rt_gc.c | item 11 |
| Heap scanned precisely from headers and the pair tag | conforms | rt_gc.c `gc_scan` | |
| Static data never scanned or freed | conforms | rt_gc.c ignores non-heap pointers | |
| Collector about 1,200 lines, single-threaded | conforms | 540 lines | |
| §7.1 `--gc=bump`: never frees, no collector in the binary | conforms | cases gc_bump, shape pay_gc_bump (new) | |
| §7.2 stack allocation, generational mode | not implemented | | "possible later additions" |

## §8 Control flow

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Native C stack, ordinary calls and returns | conforms | emitted C | |
| §8.1 proper tail calls guaranteed | conforms | cases tail_calls (all levels, tramp) | |
| §8.1 "as the report requires": the receiver of a tier-2/3 `call/cc` in tail position (R7RS 3.5) | deviates | rt_core.c `rt_call_cc` keeps its frame | item 4 |
| Self and mutual recursion of known procedures → jumps; named `let` → C loop | conforms | shape mutual, mutual_top, sum_to | |
| Other tail calls: `musttail`; one signature `obj f(obj self, intptr_t argc, obj a0..a3)` + buffer | conforms | rt.h `rt_code`, `RT_TAIL`; emitted C | |
| Trampoline fallback without the attribute | conforms | tramp mode (`--no-musttail`) | |
| §8.2 each `call/cc` site classified at compile time | conforms | analyze.c tier analysis | |
| Tier 1: jump or return, no cost | conforms | shape escape | |
| Tier 2: saved stack position + non-local jump | conforms | shape callcc_tier2 | |
| Tier 3: live stack copied to the heap and back | conforms | shape callcc_tier3; cases reentry, gen_fringe | item 10 |
| Tier 2 is used only when `k` cannot outlive the `call/cc` | deviates | DESIGN §10 | item 21: escape as an error irritant is not tracked (safe-mode error, no corruption) |
| Library analysed with the program: `k` from a `for-each` lambda stays tier 1/2 | conforms | shape escape, callcc_tier2 | |
| §8.3 `raise`, `raise-continuable`, `error`, `with-exception-handler`, `guard` on a handler stack, not general `call/cc` | conforms | lib/scheme/base/errors.scm | `guard` escapes by a tier-2 jump, no stack copy |
| `guard` clauses on top of the raising frame, unwind only on a match | conforms | run: after thunk runs after the outer handler when no clause matches | item 5 |
| `guard` as R7RS defines it: clause tests in the guard's dynamic environment | deviates | run: a test sees the raiser's `parameterize` value | item 5 |
| Uncaught error: message and irritants on stderr, non-zero exit | conforms | cases uncaught (`.err`, `.exit` 70) | |
| §8.4 `dynamic-wind` list; `parameterize` in the same list | conforms | lib/shikra/dynamic.sld; shape callcc_tier2 `wind-exit` | |
| §8.5 values to a known receiver in a C struct; unknown receivers get a heap object | conforms | shape values | |

## §9 Small-binary strategy

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Scheme level: only reachable definitions; library shaken like user code | conforms | shape hello | |
| Type level: `(display "hi")` is a string write; `(display n)` of a fixnum links only the integer printer | conforms | shape hello, sum_to_main | error paths can still pull in the printer |
| Linker level: one TU, a section per function, unreferenced sections dropped | conforms | main.c `-ffunction-sections`, `-dead_strip`/`--gc-sections` | |
| §9.1 collector: program allocates and no `--gc=bump` | conforms | shape pay_gc, pay_gc_bump, hello (new) | |
| §9.1 symbol table: `string->symbol` or `read` | conforms | shape pay_symbols, pay_read, pay_float (new) | |
| §9.1 float printing/parsing: flonum reaches a printer or `string->number` | conforms | shape pay_float, pay_symbols (new) | |
| §9.1 stack-copying continuations: some site is tier 3 | conforms | shape callcc_tier2, callcc_tier3 | |
| §9.1 datum reader: `read` reachable | conforms | shape pay_read, pay_symbols (new) | |
| §9.1 file ports: `(scheme file)` reachable | conforms | shape pay_file, pay_symbols (new) | |
| §9.1 Unicode tables: `--unicode=full` | conforms | shape pay_unicode, pay_ascii (new) | |
| §9.2 Linux: direct system calls, own entry point, static, no libc | conforms *(records)* | rt_sys.c `SHIKRA_NOLIBC`; DESIGN "Linux integration" | item 6 for `define-c` |
| §9.2 every Linux executable without libc | deviates | `emit-c` "build mode: hosted" for `define-c` (driver) | item 6 |
| §9.2 macOS: dynamic libSystem, a floor of tens of KB | conforms | hello 50,808 bytes | |
| §9.3 hello world under 30 KB Linux / about 50 KB macOS | conforms | size mode pin 52,000; Linux *(records)* | |
| §9.3 1,000–2,000 lines: 60–150 KB Linux / 100–200 KB macOS | deviates | bench/SIZES.md | item 2 |
| `shikra size` prints bytes per library and per procedure | conforms | size mode | also per runtime component |

## §10 Extensions

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Five `(shikra ...)` libraries and `define-c`; standard programs never need them | deviates | lib/shikra; `(import (shikra primitives))` and `(shikra dynamic)` are accepted | item 16: the five exist as specified, two internal ones are also visible |
| `(shikra fixnum)`: every SRFI 143 name | conforms | all 44 names imported with `(only ...)` | cases ext_fixnum |
| `(shikra flonum)`: every SRFI 144 name | conforms | all 124 names imported | cases ext_flonum; unit flmath |
| `(shikra f64vector)`: SRFI 4 `f64` names, raw doubles | conforms | all 8 names | cases ext_f64vector |
| `(shikra assume)`: SRFI 145 `assume`, checked safe, trusted `--unsafe` | conforms | shape ext_assume, ext_assume_unsafe | |
| `(shikra ffi)`: `define-c`; arguments/results converted to unboxed C values at the call | conforms | shape ext_ffi; cases ext_ffi | |
| The spec's two-line example | conforms | built as written (warnings: unused definitions); with calls: `5.0`, pid > 0, also in the REPL | |
| Portable code guards with `cond-expand` on `shikra` | conforms | run of a `cond-expand` program with `(import (shikra ffi))` | |

## §11 Toolchain

| Statement | Status | Evidence | Note |
|---|---|---|---|
| `build main.scm -o app` | conforms | compiled mode | default output name: the file's base name |
| `run main.scm`: builds at -O0 into a cache directory, runs | conforms | driver (second run reuses the executable) | |
| `repl [main.scm]` | conforms | repl/load_file | |
| `check`: expand and analyse only, no code | conforms | driver: `check` and `build` report identically | |
| `emit-c` | conforms | driver: plain `cc out.c` builds | |
| `size app` | conforms | size mode | |
| `-O` `0` / `2` / `s`, default 2 | conforms | levels mode; driver (bad values) | `run` defaults to 0 |
| `--unsafe`, default off | conforms | shape ext_*_unsafe | |
| `--gc` `immix` / `bump`, default immix | conforms | cases gc_bump; driver (bad value) | |
| `--unicode` `ascii` / `full`, default ascii | conforms | r7rs char-unicode; driver (bad value) | |
| `-I dir`, default none | conforms | driver | |
| `--cc`, default `cc` | conforms | driver (`--cc` with arguments) | `$SHIKRA_CC` overrides the default |
| `--target`, default host | deviates | driver `--target` features; bad triple refused | item 14; cross *linking* not verifiable here (no lld/zig) |
| `-g` adds source locations to run-time errors, default off | conforms | debug mode | |
| §11.2 the spec's transcript | conforms | repl/spec | echo when not a terminal (item 17) |
| Multi-line input until parentheses balance | conforms | repl/multiline | |
| `shikra repl main.scm` loads the file, its imports and definitions available | conforms | repl/load_file | |
| Errors return to the prompt; session and definitions survive | conforms | repl/errors | |
| REPL behaves like the spec's terminal session in every detail | deviates | repl/spec (echo), run: no after thunk on an uncaught error | item 17 |
| `,expand expr`, `,c name`, `,quit` | conforms | repl/expand, c, quit, meta_usage (new) | |
| No line-editing library | conforms | `otool -L` | |
| REPL accepts redefinition and `eval`; `check` decides whether a program builds | conforms | repl/redefine, eval; errors/repl_only_eval | |
| §11.3 compile-time errors: unbound identifier, unexported import | conforms | errors/unbound, unbound_toplevel, unexported, rename_missing | |
| Wrong argument count to a known procedure | conforms | errors/arity_known, arity_case_lambda, arity_named_let | |
| Provably wrong type, e.g. `(car 5)`, `(vector-ref "abc" 0)` | conforms | errors/type_car, type_vector_ref | |
| ... also when the proof goes through a procedure's parameter or result, or the code is in a `guard` body | deviates | warnings/types_boundary | item 8: a warning and a run-time error |
| Compile-time errors are the listed kinds | deviates | errors/values_count | item 19: also a literal `call-with-values` receiver with a provably wrong value count |
| `set!` on an imported binding | conforms | errors/set_imported | |
| Unused imports and definitions are warnings; warnings do not fail the build | conforms | warnings/unused, library; driver run with a warning | |
| Every message in `file:line:column` form | conforms | every tests/errors diagnostic carries a position (audit loop) | |
| Run-time errors name the procedure; `-g` adds the location | conforms | cases error_names; debug/locations | |

## §12 Worked example

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Plain C loop, one overflow check, no allocation | conforms | shape sum_to_main | |
| `sum-to` called directly, `n` known fixnum | conforms | it is inlined into the program body | |
| Named `let` becomes a loop; type checks disappear; tagged compare and add | conforms | shape sum_to, sum_to_main | |
| `display` of a known fixnum links the integer printer alone; no collector | conforms | shape sum_to_main nm checks | |
| "Where the speed runs out" | conforms | descriptive; consistent with bench/RESULTS.md | |

Differences from the sketch (it is described as a sketch): `sum-to` has no function of its own (called
once, it is inlined into `P_init`, the program body; with `--keep sum-to` it is a uniform-signature
function where `n` is unknown); the loop is a label and `goto`, not `for (;;)`; the comparison is
against the propagated constant `RT_FIX(1000000)`; the one overflow check is `rt_fxadd` (inline
`__builtin_add_overflow`, then `rt_fx_overflow`); `i + FIX(1)` is unchecked as in the sketch; output
goes through the current-output-port cell (`rt_put_fixnum(acc, CDR(rt_port_cell(1)))`,
`rt_put_newline`), not a `STDOUT` constant; `main` is the runtime's, which calls `rt_program_main`
and flushes at exit; `raise` and `error` appear in the C (the overflow path) and are removed by the
linker.

## §13 Roadmap ("done when")

| Statement | Status | Evidence | Note |
|---|---|---|---|
| M0: hello world and `fib` build and run | conforms | cases hello, fib | |
| M1: `(scheme base)` part of an R7RS suite (Chibi) passes | conforms | r7rs base 658/658 | EXCLUSIONS.md |
| M2: collect-at-every-allocation stress passes the suite | deviates | stress mode | item 9 |
| M3: suite passes in the interpreter, output identical | conforms | interp mode, r7rs `*.interp` | |
| M4: size targets met | deviates | bench/SIZES.md | item 2 |
| M4: `r7rs-benchmarks` runs, results against Chez and Gambit | conforms | bench/RESULTS.md (53 ok, 4 unsupported by design) | |
| M5: full R7RS suite passes compiled and interpreted | conforms | r7rs mode | |
| M6: suite passes on macOS arm64, Linux x86-64, Linux arm64; optional bignums | conforms *(records)* | `make test` here; DESIGN "Linux integration" (1,331 / 0 / 2 skips per configuration) | Linux not re-run |
| C99 from the first commit; interpreter as reference | conforms | | |

## §14 Risks

| Statement | Status | Evidence | Note |
|---|---|---|---|
| Fallback for tier 3 under shadow stacks: CPS for affected procedures | not implemented | | item 10; a fallback, not a requirement |
