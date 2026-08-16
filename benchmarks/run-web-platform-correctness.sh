#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-build-dev}
output=${2:-/tmp/tilefinch-web-platform}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
lab="$build/psp-browser-interactive-lab"
manifest="$root/fixtures/web-platform/selected.tsv"
mkdir -p "$output"

count=0
while IFS="$(printf '\t')" read -r name fixture expected; do
    case "$name" in ''|'#'*) continue ;; esac
    log="$output/$name.log"
    "$lab" --fixture "$root/$fixture" --ticks 4 --limit-mb 24 \
        --output "$output/$name.ppm" >"$log" 2>&1
    grep -Fq "javascript summary=\"$expected" "$log"
    grep -q '^interactive teardown=0 active=0 .*status=PASS$' "$log"
    count=$((count + 1))
done < "$manifest"

[ "$count" -gt 0 ]
printf 'selected-web-platform tests=%s status=PASS\n' "$count"
