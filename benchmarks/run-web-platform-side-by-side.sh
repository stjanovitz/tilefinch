#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-dev}
wpt_root=${2:-"$root/../wpt"}
output=${3:-/tmp/tilefinch-web-platform}

case "$build" in /*) ;; *) build="$root/$build" ;; esac
mkdir -p "$output"

"$root/benchmarks/run-web-platform-correctness.sh" \
    "$build" "$output/local"
"$root/benchmarks/run-upstream-wpt.sh" \
    "$build" "$wpt_root" "$output/upstream"

printf 'web-platform-side-by-side local=PASS upstream=%s status=COMPLETE\n' \
    "$output/upstream/results.json"
