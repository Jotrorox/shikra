#!/bin/sh
# tests/run.sh [all|reader|unit|amalg|compiled|interp|stress]   (DESIGN.md §9)
#   reader    tests/reader/NAME.scm: `shikra debug-read` output must equal NAME.out and
#             read back to itself; or, with NAME.err, fail with stderr containing it
#   unit      C-level runtime tests (tests/unit/rt_unit.c, tool mode)
#   amalg     program-mode self-check: rt.h + stand-in program + rt_*.c as ONE TU with
#             SHIKRA_PROGRAM, -Wall -Wextra -Werror; output must match
#   compiled  tests/cases/NAME.scm built with `shikra build`, stdout == NAME.out,
#             exit status == NAME.exit (default 0); tests/errors/NAME.scm: `shikra check`
#             must fail with stderr containing NAME.err
#   interp    tests/cases under the REPL interpreter (M3; skeleton)
#   stress    tests/cases built with --gc-stress, run with SHIKRA_GC_STRESS=1 (M2; skeleton)
# `all` runs every mode. Exit status is non-zero if anything failed.
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
}

run_unit() {
    if $CC -std=c99 -O2 -Wall -Wextra -Werror -Isrc tests/unit/rt_unit.c src/rt_*.c -o "$TMP/rt_unit" &&
        "$TMP/rt_unit" > "$TMP/unit.out"; then ok
    else bad "tests/unit/rt_unit.c"; fi
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

# run_cases MODE: every tests/cases/*.scm under one engine/mode.
run_cases() {
    mode=$1
    for f in tests/cases/*.scm; do
        [ -f "$f" ] || continue
        base=${f%.scm}
        want_exit=0
        [ -f "$base.exit" ] && want_exit=$(cat "$base.exit")
        case $mode in
        compiled) $SHIKRA build "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 && "$TMP/prog" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        stress) $SHIKRA build --gc-stress "$f" -o "$TMP/prog" > "$TMP/c.err" 2>&1 &&
            SHIKRA_GC_STRESS=1 "$TMP/prog" > "$TMP/c.out" 2>> "$TMP/c.err" ;;
        interp) $SHIKRA repl "$f" < /dev/null > "$TMP/c.out" 2> "$TMP/c.err" ;;
        esac
        st=$?
        if [ "$st" = "$want_exit" ] && cmp -s "$TMP/c.out" "$base.out"; then ok
        else bad "$f [$mode] (exit $st)"; head -5 "$TMP/c.err"; fi
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

mode=${1:-all}
case $mode in
reader) run_reader ;;
unit) run_unit ;;
amalg) run_amalg ;;
compiled | interp | stress) run_cases "$mode" ;;
all) run_reader; run_unit; run_amalg; run_cases compiled; run_cases interp; run_cases stress ;;
*) echo "usage: tests/run.sh [all|reader|unit|amalg|compiled|interp|stress]"; exit 2 ;;
esac
echo "tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
