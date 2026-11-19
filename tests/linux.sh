#!/bin/sh
# tests/linux.sh [arm64|amd64] [MODE...]   (DESIGN.md "M6 Linux as built")
# Runs the test suite on Linux in a podman container: Debian trixie with GCC and clang, the
# working tree (tracked and untracked files that are not ignored) copied into the container's
# own file system, so nothing is written back. Inside: the documented one-line build with both
# compilers (-Werror), then `tests/run.sh MODE` for each MODE (default: all) once per program
# compiler in $LINUX_CCS (default "gcc clang"; SHIKRA_CC selects shikra's C compiler, CC the
# one the unit tests use). Compiled programs are static executables without libc; the
# interpreter runs in the ordinary (glibc) tool.
#   arm64  native on Apple silicon
#   amd64  emulated (qemu-user binfmt in the podman machine): slow, see DESIGN.md for the
#          reduced stress set used there
# The podman machine is started if it is not running, and stopped again afterwards.
# Environment: LINUX_CCS, LINUX_IMAGE (default localhost/shikra-linux-test:ARCH), and
# LINUX_ENV (extra "NAME=value ..." passed to tests/run.sh, e.g. STRESS_SCALE=20 for amd64).
set -u
arch=${1:-arm64}
[ $# -gt 0 ] && shift
modes=${*:-all}
case $arch in arm64 | amd64) ;; *) echo "usage: tests/linux.sh [arm64|amd64] [MODE...]"; exit 2 ;; esac
cd "$(dirname "$0")/.." || exit 2
command -v podman > /dev/null 2>&1 || { echo "linux.sh: podman not found"; exit 2; }
started=
if ! podman info > /dev/null 2>&1; then podman machine start || exit 2; started=1; fi
img=${LINUX_IMAGE:-localhost/shikra-linux-test:$arch}
# qemu-user gives the guest a fixed 8 MB stack and ignores `ulimit -s` (run.sh raises it to 64 MB
# for -O0, where callcc.scm recurses 10^5 deep); QEMU_STACK_SIZE is what it honours.
case $arch-$(uname -m) in amd64-x86_64) ;; amd64-*) LINUX_ENV="QEMU_STACK_SIZE=67108864 ${LINUX_ENV:-}" ;; esac
if ! podman image exists "$img"; then
    ctx=$(mktemp -d) || exit 2
    printf '%s\n' 'FROM docker.io/library/debian:trixie-slim' \
        'RUN apt-get update && apt-get install -y --no-install-recommends gcc clang make libc6-dev binutils && rm -rf /var/lib/apt/lists/*' \
        > "$ctx/Containerfile"
    podman build --platform "linux/$arch" -t "$img" "$ctx" || exit 2
    rm -rf "$ctx"
fi
git ls-files -co --exclude-standard | COPYFILE_DISABLE=1 tar -cf - -T - |
    podman run --rm -i --platform "linux/$arch" -e LINUX_MODES="$modes" -e LINUX_CCS="${LINUX_CCS:-gcc clang}" \
        -e LINUX_ENV="${LINUX_ENV:-}" "$img" sh -c '
    mkdir -p /w && cd /w && tar -xf - 2> /dev/null || exit 2
    echo "== $(uname -m) $(uname -r): $(gcc --version | head -1); $(clang --version | head -1)"
    st=0
    for c in gcc clang; do
        if $c -std=c99 -O2 -Wall -Wextra -Werror src/*.c -o shikra; then echo "== one-line build with $c: warning-free"
        else echo "== FAIL: one-line build with $c"; st=1; fi
    done
    cc -std=c99 -O2 -Wall -Wextra src/*.c -o shikra || exit 2
    for c in $LINUX_CCS; do
        for m in $LINUX_MODES; do
            echo "== SHIKRA_CC=$c CC=$c $LINUX_ENV tests/run.sh $m"
            env SHIKRA_CC=$c CC=$c $LINUX_ENV sh tests/run.sh $m || st=1
        done
    done
    exit $st'
st=$?
[ -n "$started" ] && podman machine stop
exit $st
