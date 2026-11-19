#!/bin/bash
# bench/run.sh -- run ecraven's r7rs-benchmarks under Shikra, Chez or Gambit.
#
# Usage:
#   bench/run.sh [options] IMPL [BENCHMARK...]
#   bench/run.sh report            (same as bench/report.sh)
#
# IMPL is one of
#   shikra          SHIKRA build -O2           (safe, the default mode)
#   shikra-unsafe   SHIKRA build -O2 --unsafe
#   shikra-bignum   SHIKRA build -O2 --bignum (pi and chudnovsky run; DESIGN.md "Bignums as built")
#   shikra-interp   SHIKRA interp (bytecode interpreter; optional)
#   chez            Chez Scheme, compile-program at optimize-level 2 (safe; as upstream)
#   chez-unsafe     Chez Scheme at optimize-level 3 (unsafe)
#   gambit          gsc -exe with upstream's declarations (safe; as upstream)
#   gambit-unsafe   gsc -exe with upstream's declarations plus (not safe)
# BENCHMARK defaults to all 57 benchmarks of upstream's "all", in upstream order.
#
# Options (or environment variables):
#   -t SECS   TIMEOUT          run-time wall-clock limit per benchmark (default 120)
#   -c SECS   COMPILE_TIMEOUT  compile-time wall-clock limit per benchmark (default 600)
#   -l NAME   LABEL            name of the series in the CSV file name (default IMPL)
#   SHIKRA=/path/to/shikra     compiler binary (default: ./shikra in the repo root)
#   SHIKRA_FLAGS="..."         extra flags for `shikra build` (e.g. --gc=bump)
#   CHEZ=chez GSC=gsc          Chez / Gambit executables
#
# Results: bench/results/LABEL-YYYYMMDD-HHMM.csv; logs, generated sources and
# binaries: bench/work/LABEL-YYYYMMDD-HHMM/ (ignored by git).  Benchmarks run
# sequentially, one at a time, with the working directory set to the work
# directory (which has inputs/ and outputs/ like the upstream checkout).
#
# Requires bash 3.2 (macOS /bin/bash), perl 5 and /usr/bin/time (BSD, -l).

set -u

BENCH_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(dirname "$BENCH_DIR")
SUITE="$BENCH_DIR/r7rs-benchmarks"
LIMIT="perl $BENCH_DIR/lib/limit.pl"

ALL_BENCHMARKS="browse deriv destruc diviter divrec puzzle triangl tak takl ntakl cpstak ctak
fib fibc fibfp sum sumfp fft mbrot mbrotZ nucleic pi pnpoly ray simplex
ack array1 string sum1 cat tail wc
read1
compiler conform dynamic earley graphs lattice matrix maze mazefun nqueens paraffins parsing peval primes quicksort scheme slatex chudnovsky
nboyer sboyer gcbench mperm
equal bv2string"

usage() {
    sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

if [ "${1:-}" = "report" ]; then
    shift
    exec sh "$BENCH_DIR/report.sh" "$@"
fi

TIMEOUT=${TIMEOUT:-120}
COMPILE_TIMEOUT=${COMPILE_TIMEOUT:-600}
LABEL=${LABEL:-}
while [ $# -gt 0 ]; do
    case "$1" in
        -t) TIMEOUT=$2; shift 2 ;;
        -c) COMPILE_TIMEOUT=$2; shift 2 ;;
        -l) LABEL=$2; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "run.sh: unknown option $1" >&2; usage ;;
        *) break ;;
    esac
done
[ $# -ge 1 ] || usage
IMPL=$1; shift
BENCHMARKS=${*:-$ALL_BENCHMARKS}
[ -n "$LABEL" ] || LABEL=$IMPL

if [ ! -d "$SUITE/src" ]; then
    echo "run.sh: $SUITE missing; run bench/fetch.sh first" >&2
    exit 1
fi

SHIKRA=${SHIKRA:-$REPO_DIR/shikra}
SHIKRA_FLAGS=${SHIKRA_FLAGS:-}
CHEZ=${CHEZ:-chez}
GSC=${GSC:-gsc}

# ---------------------------------------------------------------------------
# Implementation settings

PRELUDE=/dev/null
POSTLUDE=/dev/null
STRIP_IMPORTS=no
UNSUPPORTED_FILE=/dev/null
case "$IMPL" in
    shikra|shikra-unsafe|shikra-bignum|shikra-interp)
        [ -x "$SHIKRA" ] || { echo "run.sh: no Shikra binary at $SHIKRA" >&2; exit 1; }
        SHIKRA=$(cd "$(dirname "$SHIKRA")" && pwd)/$(basename "$SHIKRA")
        PRELUDE=$BENCH_DIR/impl/shikra-prelude.scm
        POSTLUDE=$BENCH_DIR/impl/shikra-postlude.scm
        UNSUPPORTED_FILE=$BENCH_DIR/shikra-unsupported.txt
        VERSION=$(git -C "$(dirname "$SHIKRA")" rev-parse --short HEAD 2>/dev/null || echo unknown)
        if [ -n "$(git -C "$(dirname "$SHIKRA")" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
            VERSION="$VERSION+dirty"
        fi
        VERSION="shikra $VERSION"
        case "$IMPL" in
            shikra) FLAGS="build -O2 $SHIKRA_FLAGS" ;;
            shikra-unsafe) FLAGS="build -O2 --unsafe $SHIKRA_FLAGS" ;;
            shikra-bignum) FLAGS="build -O2 --bignum $SHIKRA_FLAGS" ;;
            shikra-interp) FLAGS="interp" ;;
        esac
        ;;
    chez|chez-unsafe)
        command -v "$CHEZ" >/dev/null || { echo "run.sh: $CHEZ not found" >&2; exit 1; }
        PRELUDE=$BENCH_DIR/impl/chez-prelude.scm
        POSTLUDE=$BENCH_DIR/impl/chez-postlude.scm
        if [ "$IMPL" = chez ]; then OPT=2; else OPT=3; fi
        CHEZ_OBJ=$BENCH_DIR/work/chez-r7rs-O$OPT
        mkdir -p "$CHEZ_OBJ"
        CHEZ_LIBDIRS="$BENCH_DIR/chez-r7rs::$CHEZ_OBJ"
        VERSION="Chez Scheme $("$CHEZ" --version 2>&1)"
        FLAGS="--optimize-level $OPT, compile-program, R7RS shim bench/chez-r7rs"
        ;;
    gambit|gambit-unsafe)
        command -v "$GSC" >/dev/null || { echo "run.sh: $GSC not found" >&2; exit 1; }
        if [ "$IMPL" = gambit ]; then
            PRELUDE=$SUITE/src/GambitC-prelude.scm
            FLAGS="gsc -exe; (declare (standard-bindings) (extended-bindings) (block))"
        else
            PRELUDE=$BENCH_DIR/impl/gambit-unsafe-prelude.scm
            FLAGS="gsc -exe; (declare (standard-bindings) (extended-bindings) (block) (not safe))"
        fi
        STRIP_IMPORTS=yes
        VERSION="Gambit $("$GSC" -v 2>&1 | awk '{print $1, $2}')"
        ;;
    *) echo "run.sh: unknown implementation $IMPL" >&2; usage ;;
esac

STAMP=$(date +%Y%m%d-%H%M)
WORK=$BENCH_DIR/work/$LABEL-$STAMP
CSV=$BENCH_DIR/results/$LABEL-$STAMP.csv
mkdir -p "$WORK/outputs" "$BENCH_DIR/results"
ln -sfn "$SUITE/inputs" "$WORK/inputs"

MACHINE="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sysctl -n hw.ncpu 2>/dev/null) cores, $(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 )) GB, $(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null) $(uname -m)"
SUITE_REV=$(git -C "$SUITE" rev-parse --short HEAD 2>/dev/null || echo unknown)

{
    echo "# impl=$IMPL"
    echo "# label=$LABEL"
    echo "# version=$VERSION"
    echo "# flags=$FLAGS"
    echo "# date=$(date '+%Y-%m-%d %H:%M %Z')"
    echo "# machine=$MACHINE"
    echo "# suite=r7rs-benchmarks $SUITE_REV"
    echo "# timeout=${TIMEOUT}s compile_timeout=${COMPILE_TIMEOUT}s"
    echo "benchmark,status,time_s,wall_s,compile_s,max_rss_kb,binary_bytes,note"
} > "$CSV"

echo "== $LABEL ($VERSION; $FLAGS)"
echo "== results: $CSV"
echo "== logs:    $WORK"

# ---------------------------------------------------------------------------
# Helpers

# One-line, comma- and quote-free text for the CSV note column.
clean_note() {
    tr '\n\r\t,"' '   ; ' | sed -e 's/  */ /g' -e 's/^ //' -e 's/ $//' | cut -c1-240
}

# First meaningful line of a log file.
first_line() {
    { grep -a -i -m1 -e "error" -e "exception" "$1" ||
      grep -a -v -e "^[[:space:]]*$" -e "^compiling " -e "^time: command terminated" "$1" | head -1; } 2>/dev/null
}

wall_of() { sed -n 's/.*wall=\([0-9.]*\).*/\1/p' "$1"; }
status_of() { sed -n 's/^status=\([0-9]*\).*/\1/p' "$1"; }
rss_kb_of() {
    awk '/maximum resident set size/ { printf "%d", $1 / 1024 }' "$1" 2>/dev/null
}

record() {   # name status time wall compile rss size note
    printf '%s,%s,%s,%s,%s,%s,%s,"%s"\n' "$1" "$2" "$3" "$4" "$5" "$6" "$7" \
        "$(printf '%s' "$8" | clean_note)" >> "$CSV"
    printf '%-12s %-14s time=%-10s wall=%-9s compile=%-8s rss=%sK %s\n' \
        "$1" "$2" "${3:--}" "${4:--}" "${5:--}" "${6:--}" "$(printf '%s' "$8" | cut -c1-100)"
}

unsupported_reason() {   # with --bignum, the reasons that are bignums alone do not apply
    grep "^$1|" "$UNSUPPORTED_FILE" 2>/dev/null | head -1 | cut -d'|' -f2- |
        { if [ "$IMPL" = shikra-bignum ]; then grep -v '^bignums:'; else cat; fi; }
}

# ---------------------------------------------------------------------------
# Main loop

cd "$WORK" || exit 1
for b in $BENCHMARKS; do
    if [ ! -f "$SUITE/src/$b.scm" ]; then
        record "$b" unsupported "" "" "" "" "" "no such benchmark"
        continue
    fi
    reason=$(unsupported_reason "$b")
    if [ -n "$reason" ]; then
        record "$b" unsupported "" "" "" "" "" "$reason"
        continue
    fi

    # Assemble the program as upstream's make_src_code does:
    # prelude + src/NAME.scm + src/common.scm + postlude + src/common-postlude.scm
    cat "$PRELUDE" "$SUITE/src/$b.scm" "$SUITE/src/common.scm" "$POSTLUDE" \
        "$SUITE/src/common-postlude.scm" > "$b.full.scm"
    if [ "$STRIP_IMPORTS" = yes ]; then
        # upstream gambitc_comp: sed -i -e 's/^(import (scheme.*)$//g'
        sed -e 's/^(import (scheme.*)$//' "$b.full.scm" > "$b.scm"
    else
        mv "$b.full.scm" "$b.scm"
    fi
    rm -f "$b.full.scm"

    # Compile.
    compile_s=""
    size=""
    exe=""
    case "$IMPL" in
        shikra|shikra-unsafe|shikra-bignum)
            # shellcheck disable=SC2086
            $LIMIT "$COMPILE_TIMEOUT" /dev/null "$b.compile.log" "$b.compile.log" "$b.compile.rusage" -- \
                "$SHIKRA" $FLAGS "$b.scm" -o "$b" > "$b.compile.status"
            exe="./$b" ;;
        chez|chez-unsafe)
            $LIMIT "$COMPILE_TIMEOUT" /dev/null "$b.compile.log" "$b.compile.log" "$b.compile.rusage" -- \
                "$CHEZ" -q --optimize-level "$OPT" --compile-imported-libraries \
                --libdirs "$CHEZ_LIBDIRS" --script "$BENCH_DIR/lib/chez-compile.ss" \
                "$b.scm" "$b.so" > "$b.compile.status"
            [ -f "$b.so" ] || echo "status=1 wall=0" > "$b.compile.status" ;;
        gambit|gambit-unsafe)
            $LIMIT "$COMPILE_TIMEOUT" /dev/null "$b.compile.log" "$b.compile.log" "$b.compile.rusage" -- \
                "$GSC" -o "$b" -exe "$b.scm" > "$b.compile.status"
            exe="./$b" ;;
        shikra-interp)
            echo "status=0 wall=" > "$b.compile.status" ;;
    esac
    cst=$(status_of "$b.compile.status")
    compile_s=$(wall_of "$b.compile.status")
    if [ "$cst" = 124 ]; then
        record "$b" compile-error "" "" "$compile_s" "" "" "compile timeout after ${COMPILE_TIMEOUT}s"
        continue
    elif [ "$cst" != 0 ]; then
        record "$b" compile-error "" "" "$compile_s" "" "" "$(first_line "$b.compile.log")"
        continue
    fi
    if [ -n "$exe" ] && [ -f "$exe" ]; then
        size=$(wc -c < "$exe" | tr -d ' ')
    fi

    # Run, input on stdin, as upstream: EXEC binary < inputs/NAME.input
    case "$IMPL" in
        shikra|shikra-unsafe|shikra-bignum|gambit|gambit-unsafe)
            set -- "./$b" ;;
        shikra-interp)
            set -- "$SHIKRA" interp "$b.scm" ;;
        chez|chez-unsafe)
            set -- "$CHEZ" --optimize-level "$OPT" --libdirs "$CHEZ_LIBDIRS" --program "$b.so" ;;
    esac
    $LIMIT "$TIMEOUT" "inputs/$b.input" "$b.out" "$b.err" "$b.rusage" -- "$@" > "$b.status"
    rst=$(status_of "$b.status")
    wall=$(wall_of "$b.status")
    rss=$(rss_kb_of "$b.rusage")
    csvline=$(grep -a '^+!CSVLINE!+' "$b.out" | tail -1)
    result=${csvline##*,}

    if [ "$rst" = 124 ]; then
        record "$b" timeout "" "$wall" "$compile_s" "" "$size" "killed after ${TIMEOUT}s"
    elif [ "$result" = INCORRECT ]; then
        record "$b" wrong-result "" "$wall" "$compile_s" "$rss" "$size" \
            "$(grep -a '^ERROR: returned incorrect result' "$b.out" | head -1)"
    elif [ "$rst" = 0 ] && [ -n "$csvline" ]; then
        record "$b" ok "$result" "$wall" "$compile_s" "$rss" "$size" ""
    else
        msg=$(first_line "$b.err")
        [ -n "$msg" ] || msg=$(grep -a -v -e '^Running' "$b.out" | head -1)
        if [ "$rst" -gt 128 ] 2>/dev/null; then
            msg="killed by signal $((rst - 128))${msg:+; $msg}"
        fi
        record "$b" runtime-error "" "$wall" "$compile_s" "$rss" "$size" "exit $rst: $msg"
    fi
done

echo "== done: $CSV"
