#!/bin/sh
set -eu

build_dir=${1:-build}
input_dir=${2:-benchmarks/downloads}
frame_dir=${3:-benchmark-frames}
lab="$build_dir/psp-browser-lab"

if [ ! -x "$lab" ]; then
  printf 'Build the lab first; executable not found: %s\n' "$lab" >&2
  exit 2
fi

mkdir -p "$frame_dir"

run_reader() {
  name=$1
  fixture=$2
  limit=$3
  "$lab" --fixture "$fixture" --output-dir "$frame_dir/$name-$limit" \
    --reader-profile auto --limit-mb "$limit" --js-limit-mb 8
}

expect_failure() {
  if "$@"; then
    printf 'Expected benchmark failure unexpectedly succeeded\n' >&2
    exit 1
  fi
}

run_reader wikipedia "$input_dir/wikipedia.html" 48
run_reader wikipedia "$input_dir/wikipedia.html" 16
expect_failure run_reader wikipedia "$input_dir/wikipedia.html" 12
expect_failure run_reader wikipedia "$input_dir/wikipedia.html" 11

run_reader reddit-old "$input_dir/reddit-old.html" 48
run_reader reddit-old "$input_dir/reddit-old.html" 4
expect_failure run_reader reddit-old "$input_dir/reddit-old.html" 3

"$lab" --fixture "$input_dir/reddit-current.html" \
  --output-dir "$frame_dir/reddit-current" --limit-mb 48 --js-limit-mb 8
