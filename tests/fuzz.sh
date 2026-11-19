#!/bin/sh
# tests/fuzz.sh [N [SEED]] -- randomized differential test of the optimizer (M4-1), of the flow
# analysis and its specialisations (M4-2) and of the call/cc tiers (M5): for each seed
# SEED..SEED+N-1 (default 200 seeds from 1) one program from each of tests/fuzz/gen.scm,
# gen2.scm and gen3.scm (call/cc escapes and re-entry), each run by the
# interpreter (the reference, spec §13) and compiled at -O0, -O2 and -Os (and -O0 with the
# trampoline); stdout and the exit status must agree. Failing programs are kept in
# tests/tmp/fuzz/ (FUZZ_DIR). FUZZ_GENS="gen3" runs one generator. Not part of `make test`.
set -u
cd "$(dirname "$0")/.." || exit 2
N=${1:-200}
SEED=${2:-1}
D=${FUZZ_DIR:-tests/tmp/fuzz} # separate directories let several runs go in parallel
mkdir -p "$D"
fails=0
i=0
total=0
while [ $i -lt "$N" ]; do
  s=$((SEED + i))
  i=$((i + 1))
  for gen in ${FUZZ_GENS:-gen gen2 gen3}; do
    total=$((total + 1))
    p=$D/$gen-$s.scm
    ./shikra interp tests/fuzz/$gen.scm "$s" > "$p" || { echo "generator $gen failed for seed $s"; fails=$((fails + 1)); continue; }
    ./shikra interp "$p" > "$D/ref.out" 2>&1
    echo "exit $?" >> "$D/ref.out"
    bad=
    for lev in 0 2 s tramp; do
        case $lev in
        tramp) fl="-O0 --no-musttail" ;;
        *) fl="-O$lev" ;;
        esac
        # shellcheck disable=SC2086
        if ! ./shikra build -w $fl "$p" -o "$D/prog" > "$D/build.err" 2>&1; then bad="$bad build($lev)"; continue; fi
        "$D/prog" > "$D/got.out" 2>&1
        echo "exit $?" >> "$D/got.out"
        cmp -s "$D/ref.out" "$D/got.out" || bad="$bad $lev"
    done
    if [ -n "$bad" ]; then
        fails=$((fails + 1))
        echo "$gen seed $s: differs at$bad (kept $p)"
    else
        rm -f "$p"
    fi
  done
done
echo "fuzz: $total programs, $fails failing"
[ "$fails" -eq 0 ]
