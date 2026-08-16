#!/bin/sh
set -eu

build_dir=${1:-build-bellard-current}
output_dir=${2:-interactive-acceptance-results}
lab="$build_dir/psp-browser-interactive-lab"

if [ ! -x "$lab" ]; then
  printf 'Build the interactive lab first; expected %s\n' "$lab" >&2
  exit 2
fi

mkdir -p "$output_dir"

run_once() {
  run=$1
  "$lab" \
    --fixture fixtures/interactive.html \
    --commands fixtures/lab-loop.commands \
    --loop-output-dir "$output_dir/frames-$run" \
    --no-loop-capture \
    --output "$output_dir/final-$run.ppm" \
    --limit-mb 24 > "$output_dir/run-$run.log" 2>&1
}

run_once 1
run_once 2

for run in 1 2; do
  log="$output_dir/run-$run.log"
  grep -q '^loop status=PASS ' "$log"
  grep -q '^interactive status=ok .*ticks=2 callbacks=1 pending=0 ' "$log"
  grep -q ' PSP-LA ' "$log"
  grep -q '^controller .*activations=1 edits=2 ' "$log"
  grep -Eq '^loop-anchor id="spacer" y=[1-9][0-9]* scroll=[1-9][0-9]*$' "$log"
  grep -Eq '^interaction-latency command=.* total-us=[0-9]+ .*paint-us=[0-9]+' "$log"
  grep -Eq '^interaction-latency-summary samples=[1-9][0-9]* total-us=[0-9]+ max-us=[0-9]+ ' "$log"
  grep -q '^interactive teardown=0 active=0 .*status=PASS$' "$log"
done

if ! cmp -s "$output_dir/final-1.ppm" "$output_dir/final-2.ppm"; then
  printf 'Interactive final frames are not deterministic\n' >&2
  exit 1
fi

for run in 1 2; do
  sed -n \
    -e '/^interactive status=/p' \
    -e '/^javascript summary=/p' \
    -e '/^javascript-state /p' \
    -e '/^navigation loads=/p' \
    -e '/^controller /p' \
    -e '/^interactive teardown=/p' \
    "$output_dir/run-$run.log" > "$output_dir/semantic-$run.log"
done

if ! cmp -s "$output_dir/semantic-1.log" "$output_dir/semantic-2.log"; then
  printf 'Interactive semantic traces are not deterministic\n' >&2
  diff -u "$output_dir/semantic-1.log" "$output_dir/semantic-2.log" >&2 || true
  exit 1
fi

printf 'Deterministic interactive acceptance passed: %s\n' "$output_dir"
