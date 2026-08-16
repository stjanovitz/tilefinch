#!/bin/sh
#
# Run ASan/UBSan tests in parallel without letting simultaneous failures start
# a storm of macOS atos/CoreSymbolication processes. If the parallel pass
# fails, rerun only the failed tests serially with normal symbolization, then
# preserve the original failing exit status.

set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$root/build-preset-sanitize"}
default_jobs=8
if [ "$(uname -s)" = Darwin ]; then
    # More than two concurrent ASan processes can saturate macOS
    # CoreSymbolication/LaunchServices even with symbolization disabled.
    default_jobs=2
fi
jobs=${TILEFINCH_SANITIZER_JOBS:-$default_jobs}

case "$jobs" in
    ''|*[!0-9]*|0)
        echo "TILEFINCH_SANITIZER_JOBS must be a positive integer" >&2
        exit 2
        ;;
esac

parallel_options=symbolize=0
if [ -n "${ASAN_OPTIONS:-}" ]; then
    parallel_options="${ASAN_OPTIONS}:symbolize=0"
fi

set +e
ASAN_OPTIONS=$parallel_options \
    ctest --test-dir "$build_dir" -j "$jobs" --output-on-failure
parallel_status=$?
set -e

if [ "$parallel_status" -eq 0 ]; then
    exit 0
fi

echo "Parallel sanitizer pass failed; rerunning failed tests serially for symbols." >&2
ctest --test-dir "$build_dir" --rerun-failed -j 1 --output-on-failure || true
exit "$parallel_status"
