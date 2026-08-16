#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-preset-release}
wpt_root=${2:-"$root/../wpt"}
output=${3:-/tmp/tilefinch-upstream-wpt-modern-mobile-css}

case "$build" in /*) ;; *) build="$root/$build" ;; esac

exec python3 "$root/benchmarks/run_upstream_wpt.py" \
    --lab "$build/psp-browser-interactive-lab" \
    --wpt-root "$wpt_root" \
    --manifest "$root/benchmarks/wpt/modern-mobile-css.tsv" \
    --ticks 160 \
    --output "$output"
