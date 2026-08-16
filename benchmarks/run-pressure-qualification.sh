#!/bin/sh
set -eu

build_dir=${1:-build-release}
output_dir=${2:-pressure-qualification-results}
lab="$build_dir/psp-browser-lab"
interactive="$build_dir/psp-browser-interactive-lab"

[ -x "$lab" ] && [ -x "$interactive" ] || {
  printf 'pressure qualification binaries are missing\n' >&2
  exit 2
}
mkdir -p "$output_dir/strict-frames" "$output_dir/realistic-frames"

"$lab" --fixture fixtures/mobile-viewport.html --psp-profile strict \
  --reader-profile none --scroll-all \
  --output-dir "$output_dir/strict-frames" \
  > "$output_dir/strict.log" 2>&1
"$lab" --fixture fixtures/mobile-viewport.html --psp-profile realistic \
  --reader-profile none --scroll-all \
  --output-dir "$output_dir/realistic-frames" \
  > "$output_dir/realistic.log" 2>&1
"$interactive" --fixture fixtures/mobile-viewport.html \
  --psp-profile strict --ticks 2 \
  --output "$output_dir/interactive-strict.ppm" \
  > "$output_dir/interactive-strict.log" 2>&1

grep -q '^runtime-profile=strict limit_mb=16 ' "$output_dir/strict.log"
grep -q '^benchmark status=ok ' "$output_dir/strict.log"
grep -q '^scroll mode=full .* blank=0 revisit=match$' \
  "$output_dir/strict.log"
grep -q '^pressure-summary decisions=' "$output_dir/strict.log"
grep -q '^memory teardown       current=  0.00 MiB ' \
  "$output_dir/strict.log"

grep -q '^runtime-profile=realistic limit_mb=24 ' \
  "$output_dir/realistic.log"
grep -q '^benchmark status=ok ' "$output_dir/realistic.log"
grep -q '^scroll mode=full .* blank=0 revisit=match$' \
  "$output_dir/realistic.log"
grep -q '^memory teardown       current=  0.00 MiB ' \
  "$output_dir/realistic.log"

grep -q '^runtime-profile=strict limit-mb=16 adaptive=yes history=8 session-cache=524288 tiles=4 navigation=transactional$' \
  "$output_dir/interactive-strict.log"
grep -Eq '^pressure-summary decisions=[1-9][0-9]* ' \
  "$output_dir/interactive-strict.log"
grep -q '^pressure-reason name=tile ' "$output_dir/interactive-strict.log"
grep -q '^pressure-reason name=cache ' "$output_dir/interactive-strict.log"
grep -q '^pressure-reason name=history ' "$output_dir/interactive-strict.log"
grep -q '^interactive teardown=0 .* status=PASS$' \
  "$output_dir/interactive-strict.log"

printf 'Pressure qualification passed: %s\n' "$output_dir"
