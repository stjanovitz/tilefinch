#!/bin/sh
set -eu

build_dir=${1:-build}
frame_dir=${2:-mobile-css-frames}
lab="$build_dir/psp-browser-lab"

if [ ! -x "$lab" ]; then
  printf 'Build the lab first; executable not found: %s\n' "$lab" >&2
  exit 2
fi

mkdir -p "$frame_dir"

run_mobile() {
  name=$1
  url=$2
  limit=$3
  "$lab" --url "$url" --reader-profile auto --fetch-css \
    --max-stylesheets 6 --max-css-kb 1024 --max-css-file-kb 384 \
    --limit-mb "$limit" --skip-js --output-dir "$frame_dir/$name" \
    --dump-links "$frame_dir/$name-links.tsv"
}

run_mobile wikipedia \
  https://en.wikipedia.org/wiki/PlayStation_Portable 16
run_mobile reddit-old https://old.reddit.com/r/PSP/ 8
