#!/bin/sh
# Run one libFuzzer target in fork mode, collecting every crash.
# usage: tests/fuzz/run.sh TARGET SECONDS [WORKERS] [extra libFuzzer flags...]
#   TARGET is the harness name without the fuzz_ prefix (url, css, ...).
# Work lives in build-fuzz/fuzz-work/TARGET/{corpus,artifacts}.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${TILEFINCH_FUZZ_BUILD:-$root/build-fuzz}
target=$1
seconds=$2
workers=${3:-2}
shift 2
if [ $# -gt 0 ]; then shift; fi
work=$build/fuzz-work/$target
mkdir -p "$work/corpus" "$work/artifacts"
python3 "$root/tests/fuzz/make_seeds.py" "$build/fuzz-seeds" "$target"
if [ -z "$(ls -A "$work/corpus")" ]; then
    cp "$build/fuzz-seeds/$target/"* "$work/corpus/" || true
fi
dict=
if [ -f "$build/fuzz-seeds/$target.dict" ]; then
    dict="-dict=$build/fuzz-seeds/$target.dict"
fi
# UBSan only prints by default, and fork mode discards child output, so make
# reports fatal (minus tests/fuzz/ubsan.supp) to turn them into artifacts.
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1:suppressions=$root/tests/fuzz/ubsan.supp}
export UBSAN_OPTIONS
# The PSP has ~24 MB for everything; flag anything that grows far past it.
exec "$build/tests/fuzz/fuzz_$target" $dict \
    -fork="$workers" -ignore_crashes=1 -ignore_timeouts=1 -ignore_ooms=1 \
    -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 \
    -malloc_limit_mb=64 -artifact_prefix="$work/artifacts/" \
    -print_final_stats=1 "$@" "$work/corpus"
