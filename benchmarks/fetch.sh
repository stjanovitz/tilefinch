#!/bin/sh
set -eu

output_dir=${1:-benchmarks/downloads}
mkdir -p "$output_dir"

curl --compressed -L --fail --max-time 60 \
  -A 'PSPBrowserTilefinchBenchmark/0.1 desktop research' \
  -D "$output_dir/wikipedia.headers" \
  -o "$output_dir/wikipedia.html" \
  'https://en.wikipedia.org/wiki/PlayStation_Portable'

curl --compressed -L --fail --max-time 60 \
  -A 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/138.0.0.0 Safari/537.36' \
  -D "$output_dir/reddit-current.headers" \
  -o "$output_dir/reddit-current.html" \
  'https://www.reddit.com/r/PSP/'

curl --compressed -L --fail --max-time 60 \
  -A 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Chrome/138.0.0.0 Safari/537.36' \
  -D "$output_dir/reddit-old.headers" \
  -o "$output_dir/reddit-old.html" \
  'https://old.reddit.com/r/PSP/'

date -u '+captured_at=%Y-%m-%dT%H:%M:%SZ' > "$output_dir/capture.txt"
shasum -a 256 "$output_dir/wikipedia.html" \
  "$output_dir/reddit-current.html" "$output_dir/reddit-old.html" \
  >> "$output_dir/capture.txt"

printf 'Captured pages and headers in %s\n' "$output_dir"
