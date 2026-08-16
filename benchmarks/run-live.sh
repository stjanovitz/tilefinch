#!/bin/sh
set -eu

build_dir=${1:-build}
frame_dir=${2:-live-benchmark-frames}
lab="$build_dir/psp-browser-lab"

if [ ! -x "$lab" ]; then
  printf 'Build the lab first; executable not found: %s\n' "$lab" >&2
  exit 2
fi

mkdir -p "$frame_dir"

"$lab" --url https://en.wikipedia.org/wiki/PlayStation_Portable \
  --output-dir "$frame_dir/wikipedia" --limit-mb 13 \
  --max-download-kb 4096 --dump-links "$frame_dir/wikipedia-links.tsv"

"$lab" --url https://old.reddit.com/r/PSP/ \
  --output-dir "$frame_dir/reddit-old" --limit-mb 8 \
  --max-download-kb 4096 --dump-links "$frame_dir/reddit-old-links.tsv"

# This is expected to render Reddit's verification document, not r/PSP content.
"$lab" --url https://www.reddit.com/r/PSP/ \
  --output-dir "$frame_dir/reddit-current-gate" --limit-mb 8 \
  --max-download-kb 1024 \
  --dump-links "$frame_dir/reddit-current-gate-links.tsv"
