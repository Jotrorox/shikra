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
#             stdin is NAME.in if present (else /dev/null);
#             the generated C must compile with -Wall -Wextra -Werror;
#             tests/errors/NAME.scm: `shikra check` must fail with stderr containing NAME.err
#   expand    tests/expand/NAME.scm: the program's core forms as `shikra debug-expand`
#             prints them (renaming stripped, as the REPL's ,expand) must equal NAME.out
#   r7rs      the adapted R7RS suite (tests/r7rs/README.md): each FILE.scm listed in
#             r7rs_files below, under each engine in r7rs_engines; the summary line and the
#             list of failing tests must equal tests/r7rs/expected/FILE.ENGINE (so known
#             failures are pinned and an unexpected pass is flagged too)
#   interp    tests/cases under the REPL interpreter (M3; skeleton)
#   stress    the collector's done-when test (spec §13 M2): tests/cases and the r7rs files
#             built with --gc-stress at -O2 and at -O0 and run with SHIKRA_GC_STRESS=1, i.e.
#             a full collection at every allocation; outputs must equal the normal run's
#             (same NAME.out / tests/r7rs/expected/FILE.compiled). NAME.stress-interval
#             (a number N) makes that case collect at every N-th allocation instead; each
#             use is explained in tests/cases/README-stress. The -O0 pass runs with a 64 MB
#             C stack (ulimit -s): -O0 frames are larger than 8 MB / 10^5 (callcc.scm recurses
#             10^5 deep), independent of the collector.
# `all` runs reader, unit, amalg, expand, compiled, r7rs and stress; interp (M3) is reported
# as skipped until its milestone. Exit status is non-zero if anything failed.
set -u
cd "$(dirname "$0")/.." || exit 2
SHIKRA=./shikra
CC=${CC:-cc}
TMP=tests/tmp
mkdir -p "$TMP"
pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $1"; }

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
        case $mode in
        compiled) $SHIKRA build "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 && "$TMP/prog" < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        stress) every=1
            [ -f "$base.stress-interval" ] && every=$(cat "$base.stress-interval")
            $SHIKRA build -O$opt --gc-stress "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 &&
            SHIKRA_GC_STRESS=$every "$TMP/prog" < "$in" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        interp) $SHIKRA repl "$f" < "$in" > "$TMP/c.out" 2> "$TMP/c.err" ;;
        esac
        st=$?
        if [ "$st" = "$want_exit" ] && cmp -s "$TMP/c.out" "$base.out" &&
            { [ ! -f "$base.err" ] || grep -qF -- "$(cat "$base.err")" "$TMP/c.err"; }; then ok
        else bad "$f [$mode${opt:+ -O$opt}] (exit $st)"; head -5 "$TMP/c.err"; diff "$base.out" "$TMP/c.out" | head -10; fi
        if [ "$mode" = compiled ]; then
            if $SHIKRA emit-c "$f" > "$TMP/c.c" 2> /dev/null &&
                $CC -std=c99 -Wall -Wextra -Werror -fsyntax-only "$TMP/c.c" 2> "$TMP/cc.err"; then ok
            else bad "$f (generated C is not warning-free)"; head -5 "$TMP/cc.err"; fi
        fi
    done
    if [ "$mode" = compiled ]; then
        for f in tests/errors/*.scm; do
            [ -f "$f" ] || continue
            if $SHIKRA check "$f" > /dev/null 2> "$TMP/e.err"; then bad "$f (expected a diagnostic)"
            elif grep -qF -- "$(cat "${f%.scm}.err")" "$TMP/e.err"; then ok
            else bad "$f"; head -3 "$TMP/e.err"; fi
        done
    fi
}

# The R7RS suite (tests/r7rs). Later milestones extend these lists: M3 adds the interp
# engine, M5 the other library files (see tests/r7rs/README.md for which file gates what).
r7rs_files="base"
r7rs_engines="compiled"
# r7rs_summary OUT: the final summary line, then each failing test's report (the
# "FAIL: ..." / "ERROR: ..." line and its "expected" / "but got" lines), indentation stripped.
r7rs_summary() {
    tail -n 1 "$1"
    grep -A 2 -E '^ *(FAIL|ERROR): ' "$1" | grep -v '^--$' | sed 's/^ *//'
}
run_r7rs() {
    for name in $r7rs_files; do
        f=tests/r7rs/$name.scm
        for engine in $r7rs_engines; do
            exp=tests/r7rs/expected/$name.$engine
            case $engine in
            compiled) $SHIKRA build "$f" -o "$TMP/r7rs" > "$TMP/r7.err" 2>&1 &&
                "$TMP/r7rs" < /dev/null > "$TMP/r7.out" 2>> "$TMP/r7.err" ;;
            interp) $SHIKRA repl "$f" < /dev/null > "$TMP/r7.out" 2> "$TMP/r7.err" ;;
            stress) exp=tests/r7rs/expected/$name.compiled
                $SHIKRA build -O$opt --gc-stress "$f" -o "$TMP/r7rs" > "$TMP/r7.err" 2>&1 &&
                SHIKRA_GC_STRESS=1 "$TMP/r7rs" < /dev/null > "$TMP/r7.out" 2>> "$TMP/r7.err" ;;
            esac
            r7rs_summary "$TMP/r7.out" > "$TMP/r7.sum"
            if [ ! -f "$exp" ]; then bad "$f [$engine] (no expected results file $exp)"
            elif cmp -s "$TMP/r7.sum" "$exp"; then ok
            else bad "$f [$engine${opt:+ -O$opt}]"; head -5 "$TMP/r7.err"; diff "$exp" "$TMP/r7.sum" | head -20; fi
            if [ "$engine" = compiled ]; then
                if $SHIKRA emit-c "$f" > "$TMP/r7.c" 2> /dev/null &&
                    $CC -std=c99 -Wall -Wextra -Werror -fsyntax-only "$TMP/r7.c" 2> "$TMP/cc.err"; then ok
                else bad "$f (generated C is not warning-free)"; head -5 "$TMP/cc.err"; fi
            fi
        done
    done
}

# run_stress: cases and r7rs files with a collection at every allocation, at -O2 and -O0.
run_stress() {
    for opt in 2 0; do
        (   [ $opt = 0 ] && ulimit -s 65520 2> /dev/null
            run_cases stress
            r7rs_engines=stress run_r7rs
            echo "$pass $fail" > "$TMP/stress.count" )
        set -- $(cat "$TMP/stress.count")
        pass=$1 fail=$2
    done
    opt=
}

opt=
mode=${1:-all}
case $mode in
reader) run_reader ;;
unit) run_unit ;;
amalg) run_amalg ;;
expand) run_expand ;;
compiled | interp) run_cases "$mode" ;;
stress) run_stress ;;
r7rs) run_r7rs ;;
all) run_reader; run_unit; run_amalg; run_expand; run_cases compiled; run_r7rs; run_stress
    echo "interp: skipped (REPL interpreter arrives in M3)" ;;
*) echo "usage: tests/run.sh [all|reader|unit|amalg|expand|compiled|r7rs|interp|stress]"; exit 2 ;;
esac
echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
