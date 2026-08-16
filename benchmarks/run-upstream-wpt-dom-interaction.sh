#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-dev}
wpt_root=${2:-"$root/../wpt"}
output=${3:-/tmp/tilefinch-upstream-wpt-dom-interaction}

case "$build" in /*) ;; *) build="$root/$build" ;; esac

set +e
python3 "$root/benchmarks/run_upstream_wpt.py" \
    --lab "$build/psp-browser-interactive-lab" \
    --wpt-root "$wpt_root" \
    --manifest "$root/benchmarks/wpt/dom-interaction.tsv" \
    --output "$output" \
    --strict
runner_status=$?
set -e

if [ "$runner_status" -ne 0 ]; then
    exit "$runner_status"
fi

exec python3 "$root/benchmarks/check_wpt_dom_interaction.py" \
    "$output/results.json"
