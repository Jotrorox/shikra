#!/bin/sh
# bench/fetch.sh -- fetch ecraven's r7rs-benchmarks into bench/r7rs-benchmarks
# at the pinned commit (third-party sources; ignored by git, never committed).
#
#   sh bench/fetch.sh
#
# Re-running is safe: an existing checkout is moved to the pinned commit.
set -eu

URL=https://github.com/ecraven/r7rs-benchmarks.git
REV=85f6acdc4cc4e2b857f307ba56bd0ba931dcccd1   # 2026-02-03 (master at fetch time)

DIR=$(cd "$(dirname "$0")" && pwd)/r7rs-benchmarks
if [ ! -d "$DIR/.git" ]; then
    git clone --quiet "$URL" "$DIR"
fi
cd "$DIR"
if ! git cat-file -e "$REV^{commit}" 2>/dev/null; then
    git fetch --quiet origin
fi
git -c advice.detachedHead=false checkout --quiet "$REV"
echo "r7rs-benchmarks at $(git rev-parse HEAD) in $DIR"
