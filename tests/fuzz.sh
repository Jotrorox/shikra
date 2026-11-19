#!/bin/sh
# tests/fuzz.sh [N [SEED]] -- randomized differential test of the optimizer (M4-1): N programs
# from tests/fuzz/gen.scm (default 200, seeds SEED..SEED+N-1, default 1), each run by the
# interpreter (the reference, spec §13) and compiled at -O0, -O2 and -Os (and -O0 with the
# trampoline); stdout and the exit status must agree. Failing programs are kept in
# tests/tmp/fuzz/. Not part of `make test`.
set -u
cd "$(dirname "$0")/.." || exit 2
N=${1:-200}
SEED=${2:-1}
D=tests/tmp/fuzz
mkdir -p "$D"
fails=0
i=0
while [ $i -lt "$N" ]; do
    s=$((SEED + i))
    i=$((i + 1))
    p=$D/p$s.scm
    ./shikra interp tests/fuzz/gen.scm "$s" > "$p" || { echo "generator failed for seed $s"; fails=$((fails + 1)); continue; }
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
        echo "seed $s: differs at$bad (kept $p)"
    else
        rm -f "$p"
    fi
done
echo "fuzz: $N programs, $fails failing"
[ "$fails" -eq 0 ]
