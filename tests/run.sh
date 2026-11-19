#!/bin/sh
# tests/run.sh [all|reader|unit|amalg|expand|compiled|r7rs|interp|stress]   (DESIGN.md §9)
#   reader    tests/reader/NAME.scm: `shikra debug-read` output must equal NAME.out and
#             read back to itself; or, with NAME.err, fail with stderr containing it;
#             plus: every tests/r7rs/*.scm and chibi/*.sld must read without error
#   unit      C-level runtime tests (tests/unit/rt_unit.c, tool mode)
#   amalg     program-mode self-check: rt.h + stand-in program + rt_*.c as ONE TU with
#             SHIKRA_PROGRAM, -Wall -Wextra -Werror; output must match
#   compiled  tests/cases/NAME.scm built with `shikra build`, stdout == NAME.out,
#             exit status == NAME.exit (default 0), stderr contains NAME.err if present,
#             stdin is NAME.in if present (else /dev/null); NAME.flags: extra shikra flags
#             (e.g. --unicode=full, also given to the interpreter); NAME.args: the program's
#             arguments (also for interp);
#             the generated C must compile with -Wall -Wextra -Werror;
#             tests/errors/NAME.scm: `shikra check` must fail with stderr containing NAME.err;
#             tests/warnings/NAME.scm: `shikra check` must succeed with stderr equal to NAME.warn
#   expand    tests/expand/NAME.scm: the program's core forms as `shikra debug-expand`
#             prints them (renaming stripped, as the REPL's ,expand) must equal NAME.out
#   r7rs      the adapted R7RS suite (tests/r7rs/README.md): each FILE:ENGINE[:FLAG] in r7rs_runs
#             below (FLAG: an extra shikra flag such as --unicode=full); the summary line and the list of failing tests must equal
#             tests/r7rs/expected/FILE.ENGINE (so known failures are pinned and an unexpected
#             pass is flagged too)
#   interp    tests/cases/NAME.scm run by the interpreter (`shikra interp`, the hidden batch
#             runner): stdout, stderr expectation and exit status must equal the compiled
#             run's expectations (the same NAME.out / NAME.exit / NAME.err; there are no
#             per-engine expected files). NAME.compiled-only (one line: the reason) marks a
#             case whose behaviour legitimately differs in the interpreter; it is reported
#             as skipped with its reason, never silently. Plus tests/repl/NAME.in piped into
#             `shikra repl` (with NAME.args: the command's arguments): the whole output,
#             stdout and stderr together, must equal NAME.out
#   stress    the collector's done-when test (spec §13 M2): tests/cases and the r7rs files
#             built with --gc-stress at -O2 and at -O0 and run with SHIKRA_GC_STRESS=1, i.e.
#             a full collection at every allocation; outputs must equal the normal run's
#             (same NAME.out / tests/r7rs/expected/FILE.compiled). NAME.stress-interval
#             (a number N) makes that case collect at every N-th allocation instead (DESIGN.md
#             M2 "Stress mode" says why for each). The -O0 pass runs with a 64 MB C stack
#             (ulimit -s): -O0 frames are larger than 8 MB / 10^5 (callcc.scm recurses 10^5
#             deep), independent of the collector. Then the interpreter (M3): every case and
#             tests/r7rs/base.scm run by `shikra interp` with SHIKRA_GC_STRESS=1 (or
#             NAME.interp-stress-interval; DESIGN.md M3 "Stress mode"). Since M5 every compiled
#             r7rs run (r7rs_runs) is part of the stress pass, and every interpreted one of the
#             interpreter stress pass.
# `all` runs reader, unit, amalg, expand, compiled, r7rs, interp and stress. Exit status is
# non-zero if anything failed.
set -u
cd "$(dirname "$0")/.." || exit 2
SHIKRA=./shikra
CC=${CC:-cc}
# On Linux x86-64/arm64 compiled programs are static and use no libc (M6): the warning check of
# the generated C then compiles that variant (DESIGN.md "M6 Linux as built").
CHECK_FLAGS=
case "$(uname -s)-$(uname -m)" in
Linux-x86_64 | Linux-aarch64) CHECK_FLAGS="-DSHIKRA_NOLIBC=1 -ffreestanding" ;;
esac
TMP=tests/tmp
mkdir -p "$TMP"
pass=0
fail=0
skip=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $1"; }
skipped() { skip=$((skip + 1)); echo "skipped: $1"; }

run_reader() {
    for f in tests/reader/*.scm; do
        base=${f%.scm}
        if [ -f "$base.out" ]; then
            if $SHIKRA debug-read "$f" > "$TMP/r.out" 2> "$TMP/r.err" && cmp -s "$TMP/r.out" "$base.out"; then ok
            else bad "$f"; diff "$base.out" "$TMP/r.out" | head -20; head -5 "$TMP/r.err"; fi
            if $SHIKRA debug-read "$TMP/r.out" > "$TMP/r2.out" 2>&1 && cmp -s "$TMP/r.out" "$TMP/r2.out"; then ok
            else bad "$f (write/read round trip)"; diff "$TMP/r.out" "$TMP/r2.out" | head -10; fi
        elif [ -f "$base.err" ]; then
            if $SHIKRA debug-read "$f" > /dev/null 2> "$TMP/r.err"; then bad "$f (expected an error)"
            elif grep -qF -- "$(cat "$base.err")" "$TMP/r.err"; then ok
            else bad "$f"; echo "  expected: $(cat "$base.err")"; echo "  got:      $(cat "$TMP/r.err")"; fi
        fi
    done
    # smoke: every adapted R7RS test file must read without error
    for f in tests/r7rs/*.scm tests/r7rs/chibi/*.sld; do
        [ -f "$f" ] || continue
        if $SHIKRA debug-read "$f" > /dev/null 2> "$TMP/r.err"; then ok; else bad "$f (read)"; cat "$TMP/r.err"; fi
    done
}

run_unit() {
    if $CC -std=c99 -O2 -Wall -Wextra -Werror -Isrc tests/unit/rt_unit.c src/rt_*.c -o "$TMP/rt_unit" &&
        rm -f "$TMP/flush.txt" && "$TMP/rt_unit" > "$TMP/unit.out"; then ok
    else bad "tests/unit/rt_unit.c"; fi
    if [ "$(cat "$TMP/flush.txt" 2>/dev/null)" = "flushed at exit" ]; then ok
    else bad "tests/unit/rt_unit.c (open file port not flushed at exit)"; fi
    grep -v '^rt_unit:' "$TMP/unit.out" 2>/dev/null
    tail -1 "$TMP/unit.out" 2>/dev/null
    # rt_math.c (M6) against the host libm: >= 10^5 arguments per range, special cases
    if $CC -std=c99 -O2 -Wall -Wextra -Werror -Isrc tests/unit/math_unit.c -lm -o "$TMP/math_unit" &&
        "$TMP/math_unit" > "$TMP/math.out"; then ok
    else bad "tests/unit/math_unit.c"; grep -v '^math_unit:' "$TMP/math.out"; fi
    grep '^math_unit:' "$TMP/math.out"
}

run_amalg() {
    { echo '#define SHIKRA_PROGRAM 1'; cat src/rt.h tests/unit/amalg_prog.c.in src/rt_*.c; } > "$TMP/amalg.c"
    if $CC -std=c99 -O2 -Wall -Wextra -Werror "$TMP/amalg.c" -o "$TMP/amalg"; then
        "$TMP/amalg" > "$TMP/amalg.out" 2> "$TMP/amalg.err"
        st=$?
        printf 'hello, world\n(6765 0.25)\n(a "b" #\\c . 1.5)\n' > "$TMP/amalg.exp"
        if [ $st -eq 70 ] && cmp -s "$TMP/amalg.out" "$TMP/amalg.exp" &&
            [ "$(cat "$TMP/amalg.err")" = "Error: car: expected pair 5" ]; then ok
        else bad "program-mode amalgamation (exit $st)"; cat "$TMP/amalg.out" "$TMP/amalg.err"; fi
    else bad "program-mode amalgamation does not compile warning-free"; fi
}

run_expand() {
    for f in tests/expand/*.scm; do
        [ -f "$f" ] || continue
        if $SHIKRA debug-expand "$f" 2> "$TMP/x.err" | sed -n '/^;; program$/,$p' | tail -n +2 > "$TMP/x.out" &&
            cmp -s "$TMP/x.out" "${f%.scm}.out"; then ok
        else bad "$f"; diff "${f%.scm}.out" "$TMP/x.out" | head -10; head -3 "$TMP/x.err"; fi
    done
}

# run_cases MODE: every tests/cases/*.scm under one engine/mode.
run_cases() {
    mode=$1
    for f in tests/cases/*.scm; do
        [ -f "$f" ] || continue
        base=${f%.scm}
        want_exit=0
        [ -f "$base.exit" ] && want_exit=$(cat "$base.exit")
        in=/dev/null
        [ -f "$base.in" ] && in=$base.in
        flags=
        [ -f "$base.flags" ] && flags=$(cat "$base.flags")
        args=
        [ -f "$base.args" ] && args=$(cat "$base.args")
        case $mode in
        interp | interp-stress)
            if [ -f "$base.compiled-only" ]; then skipped "$f [$mode]: compiled-only: $(cat "$base.compiled-only")"; continue; fi ;;
        esac
        # shellcheck disable=SC2086
        case $mode in
        compiled) $SHIKRA build -O${opt:-2} $flags "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 && "$TMP/prog" $args < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        tramp) $SHIKRA build -O${opt:-0} --no-musttail $flags "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 && "$TMP/prog" $args < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        gcc) $SHIKRA build -O${opt:-2} --cc "$GCC" $flags "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 && "$TMP/prog" $args < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        gcctramp) $SHIKRA build -O${opt:-0} --no-musttail --cc "$GCC" $flags "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 && "$TMP/prog" $args < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        stress) every=1
            [ -f "$base.stress-interval" ] && every=$(cat "$base.stress-interval")
            $SHIKRA build -O$opt --gc-stress $flags "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 &&
            SHIKRA_GC_STRESS=$every "$TMP/prog" $args < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        interp) $SHIKRA interp $flags "$f" $args < "$in" > "$TMP/c.out" 2> "$TMP/c.err" ;;
        interp-stress) every=1
            [ -f "$base.interp-stress-interval" ] && every=$(cat "$base.interp-stress-interval")
            SHIKRA_GC_STRESS=$every $SHIKRA interp $flags "$f" $args < "$in" > "$TMP/c.out" 2> "$TMP/c.err" ;;
        esac
        st=$?
        if [ "$st" = "$want_exit" ] && cmp -s "$TMP/c.out" "$base.out" &&
            { [ ! -f "$base.err" ] || grep -qF -- "$(cat "$base.err")" "$TMP/c.err"; }; then ok
        else bad "$f [$mode${opt:+ -O$opt}] (exit $st)"; head -5 "$TMP/c.err"; diff "$base.out" "$TMP/c.out" | head -10; fi
        if [ "$mode" = compiled ]; then
            # shellcheck disable=SC2086
            if $SHIKRA emit-c -O${opt:-2} $flags "$f" > "$TMP/c.c" 2> /dev/null &&
                $CC -std=c99 -Wall -Wextra -Werror $CHECK_FLAGS -fsyntax-only "$TMP/c.c" 2> "$TMP/cc.err"; then ok
            else bad "$f (generated C is not warning-free)"; head -5 "$TMP/cc.err"; fi
        fi
    done
    if [ "$mode" = compiled ] && [ "${opt:-2}" = 2 ]; then
        for f in tests/errors/*.scm; do
            [ -f "$f" ] || continue
            if $SHIKRA check "$f" > /dev/null 2> "$TMP/e.err"; then bad "$f (expected a diagnostic)"
            elif grep -qF -- "$(cat "${f%.scm}.err")" "$TMP/e.err"; then ok
            else bad "$f"; head -3 "$TMP/e.err"; fi
        done
        # warnings (spec §11.3): check succeeds, stderr equals NAME.warn; -w silences them
        for f in tests/warnings/*.scm; do
            [ -f "$f" ] || continue
            if $SHIKRA check "$f" > /dev/null 2> "$TMP/w.err" && cmp -s "$TMP/w.err" "${f%.scm}.warn"; then ok
            else bad "$f (warnings)"; diff "${f%.scm}.warn" "$TMP/w.err" | head -5; fi
            if $SHIKRA check -w "$f" > /dev/null 2> "$TMP/w.err" && [ ! -s "$TMP/w.err" ]; then ok
            else bad "$f (-w)"; head -3 "$TMP/w.err"; fi
        done
    fi
}

# REPL transcripts: tests/repl/NAME.in piped into `shikra repl [NAME.args]`, run in
# tests/repl; stdout and stderr together must equal NAME.out (with stdin not a terminal the
# REPL echoes each input line after the prompt, so the output reads like the session).
run_repl() {
    for f in tests/repl/*.in; do
        [ -f "$f" ] || continue
        base=${f%.in}
        args=
        [ -f "$base.args" ] && args=$(cat "$base.args")
        # shellcheck disable=SC2086
        # C variable and label numbers in ,c output (v_x_30, j31) depend on the library sources: normalised
        (cd tests/repl && ../../shikra repl $args < "$(basename "$f")" 2>&1) |
            sed -E 's/(v_[A-Za-z0-9_]*)_[0-9]+/\1_N/g; s/(^| )j[0-9]+:/\1jN:/; s/goto j[0-9]+;/goto jN;/' > "$TMP/repl.out"
        if cmp -s "$TMP/repl.out" "$base.out"; then ok
        else bad "$f [repl]"; diff "$base.out" "$TMP/repl.out" | head -20; fi
    done
}

# The R7RS suite (tests/r7rs): FILE:ENGINE[:FLAG] (see tests/r7rs/README.md for which file
# gates what). eval.scm is REPL-only (spec §3.1); char-unicode.scm needs --unicode=full.
r7rs_files="base char inexact lazy case-lambda file read write process-context time"
r7rs_runs=
for r7f in $r7rs_files; do r7rs_runs="$r7rs_runs $r7f:compiled $r7f:interp"; done
r7rs_runs="$r7rs_runs char-unicode:compiled:--unicode=full char-unicode:interp:--unicode=full eval:interp"
# r7rs_summary OUT: the final summary line, then each failing test's report (the
# "FAIL: ..." / "ERROR: ..." line and its "expected" / "but got" lines), indentation stripped.
r7rs_summary() {
    tail -n 1 "$1"
    grep -A 2 -E '^ *(FAIL|ERROR): ' "$1" | grep -v '^--$' | sed 's/^ *//'
}
run_r7rs() {
    for run in $r7rs_runs; do
        name=${run%%:*}
        engine=${run#*:}
        flag=
        case $engine in *:*) flag=${engine#*:}; engine=${engine%%:*} ;; esac
        f=tests/r7rs/$name.scm
        exp=tests/r7rs/expected/$name.$engine
        # shellcheck disable=SC2086
        case $engine in
        compiled) $SHIKRA build -O${opt:-2} $flag "$f" -o "$TMP/r7rs" > "$TMP/r7.err" 2>&1 &&
            "$TMP/r7rs" < /dev/null > "$TMP/r7.out" 2>> "$TMP/r7.err" ;;
        interp) $SHIKRA interp $flag "$f" < /dev/null > "$TMP/r7.out" 2> "$TMP/r7.err" ;;
        stress) exp=tests/r7rs/expected/$name.compiled
            $SHIKRA build -O$opt --gc-stress $flag "$f" -o "$TMP/r7rs" > "$TMP/r7.err" 2>&1 &&
            SHIKRA_GC_STRESS=1 "$TMP/r7rs" < /dev/null > "$TMP/r7.out" 2>> "$TMP/r7.err" ;;
        tramp) exp=tests/r7rs/expected/$name.compiled
            $SHIKRA build -O${opt:-0} --no-musttail $flag "$f" -o "$TMP/r7rs" > "$TMP/r7.err" 2>&1 &&
            "$TMP/r7rs" < /dev/null > "$TMP/r7.out" 2>> "$TMP/r7.err" ;;
        interp-stress) exp=tests/r7rs/expected/$name.interp
            SHIKRA_GC_STRESS=1 $SHIKRA interp $flag "$f" < /dev/null > "$TMP/r7.out" 2> "$TMP/r7.err" ;;
        esac
        r7rs_summary "$TMP/r7.out" > "$TMP/r7.sum"
        if [ ! -f "$exp" ]; then bad "$f [$engine] (no expected results file $exp)"
        elif cmp -s "$TMP/r7.sum" "$exp"; then ok
        else bad "$f [$engine${opt:+ -O$opt}]"; head -5 "$TMP/r7.err"; diff "$exp" "$TMP/r7.sum" | head -20; fi
        if [ "$engine" = compiled ]; then
            # shellcheck disable=SC2086
            if $SHIKRA emit-c -O${opt:-2} $flag "$f" > "$TMP/r7.c" 2> /dev/null &&
                $CC -std=c99 -Wall -Wextra -Werror $CHECK_FLAGS -fsyntax-only "$TMP/r7.c" 2> "$TMP/cc.err"; then ok
            else bad "$f (generated C is not warning-free)"; head -5 "$TMP/cc.err"; fi
        fi
    done
}

# run_levels: the compiled cases and the compiled r7rs runs at -O2, -O0 and -Os (M4-1: the
# optimizer's budgets differ per level; every level must give the same results).
compiled_runs=$(echo "$r7rs_runs" | tr ' ' '\n' | grep ':compiled' | tr '\n' ' ')
# The -O0 pass raises the C stack to 64 MB, as the stress pass does (callcc.scm recurses 10^5
# deep and -O0 frames are larger).
run_levels() {
    for opt in 2 0 s; do
        (   [ $opt = 0 ] && ulimit -s 65520 2> /dev/null
            run_cases compiled
            if [ "${1:-}" = r7rs ]; then
                if [ $opt = 2 ]; then run_r7rs; else r7rs_runs=$compiled_runs; run_r7rs; fi
            fi
            echo "$pass $fail" > "$TMP/levels.count" )
        set -- "${1:-}" $(cat "$TMP/levels.count")
        pass=$2 fail=$3
    done
    opt=
}
# run_tramp: tail calls through the trampoline (--no-musttail; spec §8.1 fallback): every case
# and every compiled r7rs run, at -O0 (no sibling-call optimisation to hide a missing tail call).
run_tramp() {
    (   opt=0
        ulimit -s 65520 2> /dev/null
        run_cases tramp
        r7rs_runs=$(echo "$compiled_runs" | tr ' ' '\n' | sed -n 's/:compiled/:tramp/p' | tr '\n' ' ')
        run_r7rs
        echo "$pass $fail" > "$TMP/tramp.count" )
    set -- $(cat "$TMP/tramp.count")
    pass=$1 fail=$2
}
# gcc mode: the compiled cases built by GCC (GCC=gcc-16 by default) at -O2, -O0 and -O0 with the
# trampoline; not part of all (it needs that compiler).
GCC=${GCC:-gcc-16}
command -v "$GCC" > /dev/null 2>&1 || GCC=gcc

# run_shape: tests/shape/NAME.scm, structural properties of the emitted C (M4-1, spec §12): the
# program is compiled at each level of NAME.check's "levels" line (default: 2 s 0), must run with
# output NAME.out, and its generated functions (between the literals and rt_program_main) must
# satisfy NAME.check, one directive per line ('#' comments; "flags ..." adds shikra flags, e.g.
# --keep NAME to keep a definition as its own function so its body can be selected):
#   all          select every generated function
#   fn REGEX     select the functions whose header line matches REGEX
#   fnof REGEX   select the functions containing a line that matches REGEX
#   loops N      the selection has at least N loops (a label and a later goto back to it); select
#                the loop bodies (label to last goto)
#   no REGEX     no line of the selection matches          has REGEX   some line does
#   count N REGEX  exactly N lines of the selection match
#   nm-no REGEX  no symbol defined in the binary (nm) matches;  nm-has REGEX  some symbol does (M4-2:
#                what is linked, spec §9.1)
# REGEX is an extended regular expression without spaces (use [[:space:]]; "(" as [(]).
shape_select() { # $1 directive, $2 argument; selection in $TMP/sel, program in $TMP/prog.fns
    case $1 in
    all) cp "$TMP/prog.fns" "$TMP/sel" ;;
    fn) awk -v re="$2" '/^static [A-Za-z0-9_]+ [A-Za-z0-9_]+\(.*\) \{$/ { on = ($0 ~ re) } on { print } /^}$/ { on = 0 }' "$TMP/prog.fns" > "$TMP/sel" ;;
    fnof) awk -v re="$2" '/^static [A-Za-z0-9_]+ [A-Za-z0-9_]+\(.*\) \{$/ { n = 0; hit = 0 } { buf[n++] = $0; if ($0 ~ re) hit = 1 }
              /^}$/ { if (hit) for (i = 0; i < n; i++) print buf[i]; n = 0; hit = 0 }' "$TMP/prog.fns" > "$TMP/sel" ;;
    loops) awk -v want="$2" '{ l[NR] = $0 } END {
              for (a = 1; a <= NR; a++) if (match(l[a], /^ *(j[0-9]+|entry):;$/)) {
                  lab = l[a]; gsub(/[ :;]/, "", lab); last = 0
                  for (b = a + 1; b <= NR; b++) if (index(l[b], "goto " lab ";")) last = b
                  if (last) { found++; for (b = a; b <= last; b++) print l[b] } }
              if (found < want) { print "SHAPE-FAIL: " found " loops, expected " want > "/dev/stderr"; exit 1 } }' "$TMP/sel" > "$TMP/sel2" &&
          mv "$TMP/sel2" "$TMP/sel" ;;
    esac
}
# Symbols defined in a binary, without the suffixes of GCC's clones (rt_put_fixnum.isra.0).
nm_defined() { nm "$1" | awk '$2 !~ /^[Uu]$/ { print $3 }' | sed -E 's/[.](isra|constprop|part|cold|lto_priv)([.][0-9a-z_]+)*$//'; }
run_shape() {
    for f in tests/shape/*.scm; do
        [ -f "$f" ] || continue
        base=${f%.scm}
        levels=$(sed -n 's/^levels //p' "$base.check")
        sflags=$(sed -n 's/^flags //p' "$base.check")
        for lev in ${levels:-2 s 0}; do
            what="$f [-O$lev]"
            # shellcheck disable=SC2086
            if ! { $SHIKRA build -O$lev -w $sflags "$f" -o "$TMP/shape" > "$TMP/shape.err" 2>&1 && "$TMP/shape" > "$TMP/shape.out" 2>&1 &&
                   cmp -s "$TMP/shape.out" "$base.out"; }; then
                bad "$what (build or output)"; head -5 "$TMP/shape.err"; continue
            fi
            # shellcheck disable=SC2086
            $SHIKRA emit-c -O$lev -w $sflags "$f" | sed -n '/^\/\* ==== program ==== \*\//,/^static void rt_program_main/p' > "$TMP/prog.fns"
            cp "$TMP/prog.fns" "$TMP/sel"
            failed=
            while read -r dir arg rest; do
                case $dir in
                '' | '#'* | levels | flags) continue ;;
                all | fn | fnof | loops) shape_select "$dir" "$arg" 2> "$TMP/shape.err" || failed="$dir $arg: $(cat "$TMP/shape.err")" ;;
                no) if grep -Eq -- "$arg" "$TMP/sel"; then failed="no $arg: $(grep -E -- "$arg" "$TMP/sel" | head -2)"; fi ;;
                has) grep -Eq -- "$arg" "$TMP/sel" || failed="has $arg" ;;
                count) [ "$(grep -Ec -- "$rest" "$TMP/sel")" = "$arg" ] || failed="count $arg $rest: $(grep -Ec -- "$rest" "$TMP/sel")" ;;
                nm-no) if nm_defined "$TMP/shape" | grep -Eq -- "$arg"; then
                           failed="nm-no $arg: $(nm_defined "$TMP/shape" | grep -E -- "$arg" | head -3 | tr '\n' ' ')"; fi ;;
                nm-has) nm_defined "$TMP/shape" | grep -Eq -- "$arg" || failed="nm-has $arg" ;;
                *) failed="unknown directive $dir" ;;
                esac
                [ -n "$failed" ] && break
            done < "$base.check"
            if [ -z "$failed" ]; then ok; else bad "$what: $failed"; fi
        done
    done
    # spec §1.1, §9.3: hello world under 30 KB, statically linked, on Linux (no libc, M6)
    if [ -n "$CHECK_FLAGS" ]; then
        for lev in 2 s; do
            if $SHIKRA build -O$lev tests/cases/hello.scm -o "$TMP/hello" > "$TMP/shape.err" 2>&1 &&
                "$TMP/hello" | cmp -s - tests/cases/hello.out && [ "$(wc -c < "$TMP/hello")" -lt 30000 ]; then ok
            else bad "hello world -O$lev: $(wc -c < "$TMP/hello" 2> /dev/null) bytes (Linux: under 30 KB, spec §9.3)"; head -3 "$TMP/shape.err"; fi
        done
    fi
}

# run_stress: cases and r7rs files with a collection at every allocation, at -O2 and -O0.
run_stress() {
    for opt in 2 0; do
        (   [ $opt = 0 ] && ulimit -s 65520 2> /dev/null
            run_cases stress
            r7rs_runs=$(echo "$r7rs_runs" | tr ' ' '\n' | sed -n 's/:compiled/:stress/p' | tr '\n' ' ')
            run_r7rs
            echo "$pass $fail" > "$TMP/stress.count" )
        set -- $(cat "$TMP/stress.count")
        pass=$1 fail=$2
    done
    opt=
    run_cases interp-stress
    saved_runs=$r7rs_runs
    r7rs_runs=$(echo "$r7rs_runs" | tr ' ' '\n' | sed -n 's/:interp/:interp-stress/p' | tr '\n' ' ')
    run_r7rs
    r7rs_runs=$saved_runs
}

opt=
mode=${1:-all}
case $mode in
reader) run_reader ;;
unit) run_unit ;;
amalg) run_amalg ;;
expand) run_expand ;;
compiled) run_levels ;;
compiled2) opt=2; run_cases compiled ;;
interp) run_cases interp; run_repl ;;
stress) run_stress ;;
r7rs) run_r7rs; for opt in 0 s; do saved_runs=$r7rs_runs; r7rs_runs=$compiled_runs; run_r7rs; r7rs_runs=$saved_runs; done; opt= ;;
levels) run_levels r7rs ;;
tramp) run_tramp ;;
gcc) opt=2; run_cases gcc
    ( opt=0; ulimit -s 65520 2> /dev/null; run_cases gcc; echo "$pass $fail" > "$TMP/gcc.count" ); set -- $(cat "$TMP/gcc.count"); pass=$1 fail=$2
    ( opt=0; ulimit -s 65520 2> /dev/null; run_cases gcctramp; echo "$pass $fail" > "$TMP/gcc.count" ); set -- $(cat "$TMP/gcc.count"); pass=$1 fail=$2
    opt= ;;
shape) run_shape ;;
all) run_reader; run_unit; run_amalg; run_expand; run_levels r7rs; run_shape; run_cases interp; run_repl; run_tramp
    run_stress ;;
*) echo "usage: tests/run.sh [all|reader|unit|amalg|expand|compiled|compiled2|r7rs|interp|stress|tramp|gcc|shape]"; exit 2 ;;
esac
if [ "$skip" -gt 0 ]; then echo "tests: $pass passed, $fail failed, $skip skipped (see above)"
else echo "tests: $pass passed, $fail failed"; fi
[ "$fail" -eq 0 ]
