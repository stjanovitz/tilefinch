#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-release}
wpt_root=${2:-"$root/../wpt"}
output=${3:-/tmp/tilefinch-upstream-wpt-customizable-select}

case "$build" in /*) ;; *) build="$root/$build" ;; esac

exec python3 "$root/benchmarks/run_upstream_wpt.py" \
    --lab "$build/psp-browser-interactive-lab" \
    --wpt-root "$wpt_root" \
    --manifest "$root/benchmarks/wpt/customizable-select.tsv" \
    --output "$output"
